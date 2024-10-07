#include <string.h>
#include <math.h>
#include <zstd.h>
#include <assert.h>
#if _WIN32
  #include <winsock2.h>
  #include <windows.h>
  #define usleep(x) Sleep((x)/1000)
#else
  #include <pthread.h>
  #include <netdb.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
#endif

#ifdef LIBREMOTE_STANDALONE
  #include <lua.h>
  #include <lauxlib.h>
  #include <lualib.h>
#else
  #define LITE_XL_PLUGIN_ENTRYPOINT
  #include <lite_xl_plugin_api.h>
#endif


typedef struct { uint8_t b, g, r, a; } RenColor;
typedef struct { int x, y, width, height; } RenRect;

typedef enum {
  PACKET_NONE,
  PACKET_COMMAND_BUFFER,
  PACKET_FONT_REGISTER,
  PACKET_EVENT
} EPacketType;

#define FONT_FALLBACK_MAX 5

enum CommandType { SET_CLIP, DRAW_TEXT, DRAW_RECT };

typedef struct {
  size_t size;
  size_t capacity;
  size_t length;
  char* data;
} array_t;

size_t array_reserve(array_t* array, size_t length) {
  if (array->capacity < length) {
    if (array->capacity == 0)
      array->capacity = 1;
    while (array->capacity < length) 
      array->capacity <<= 1;
    array->data = realloc(array->data, array->capacity);
  }
  return length;
}
size_t array_append(array_t* array, const void* data, size_t length) {
  if (array->size == 0)
    array->size = length;
  array_reserve(array, length + array->length);
  memcpy(&array->data[array->length], data, length);
  array->length += length;
  return array->length;
}
void array_clear(array_t* array) { array->length = 0; }
size_t array_length(array_t* array) { return array->length / array->size; }
void array_shift(array_t* array, size_t size) {
  if (size) {
    memmove(array->data, &array->data[size], array->length - size);
    array->length -= size;
  }
}

typedef struct {
  enum CommandType type;
  uint32_t size;
  RenRect command[];
} Command;

typedef struct {
  Command command;
  RenRect rect;
} SetClipCommand;

typedef struct {
  Command command;
  RenColor color;
  int fonts[FONT_FALLBACK_MAX];
  float text_x;
  int y;
  size_t len;
  int8_t tab_size;
  char text[];
} DrawTextCommand;

typedef struct {
  Command command;
  RenRect rect;
  RenColor color;
} DrawRectCommand;


static RenRect rect_to_grid(lua_Number x, lua_Number y, lua_Number w, lua_Number h) {
  int x1 = (int) (x + 0.5), y1 = (int) (y + 0.5);
  int x2 = (int) (x + w + 0.5), y2 = (int) (y + h + 0.5);
  return (RenRect) {x1, y1, x2 - x1, y2 - y1};
}

#define HASH_INITIAL 2166136261

static void hash(unsigned *h, const void *data, int size) {
  const unsigned char *p = data;
  while (size--) {
    *h = (*h ^ *p++) * 16777619;
  }
}

typedef struct {
  array_t buffer;
  unsigned int checksum;
} SRencache;

typedef struct {
  int index;
  struct RenFont* font;
} SFont;

typedef struct {
  int fd;
  EPacketType incoming_packet_type;
  array_t incoming_compressed_buffer;
  array_t incoming_buffer;
  array_t outgoing_compressed_buffer;
  array_t outgoing_buffer;
} SDuplex;

typedef struct {
  SDuplex duplex;
  int listening;
  array_t registered_fonts;
  SRencache rencache;
  unsigned int previous_rencache_checksum;
} SServer;

typedef struct {
  SDuplex duplex;
  int font_table;
  array_t registered_fonts;
}  SClient;


static int get_font_index(SServer* server, struct RenFont* font) {
  for (int i = 0; i < server->registered_fonts.length / server->registered_fonts.size; ++i) {
    if (((SFont*)server->registered_fonts.data)[i].font == font)
      return ((SFont*)server->registered_fonts.data)[i].index;
  }
  return -1;
}



static int send_compressed_buffer(SDuplex* duplex, EPacketType type, array_t* buffer) {
  if (!duplex->fd) {
    array_clear(buffer);
    return -1;
  }
  size_t length = array_reserve(&duplex->outgoing_compressed_buffer, ZSTD_compressBound(buffer->length) + sizeof(int) + sizeof(char));
  duplex->outgoing_compressed_buffer.data[0] = type;
  size_t compressed_length = ZSTD_compress(&duplex->outgoing_compressed_buffer.data[sizeof(char) + sizeof(int)], length - sizeof(char) - sizeof(int), buffer->data, buffer->length, 1);
  array_shift(buffer, buffer->length);
  *((int*)&duplex->outgoing_compressed_buffer.data[sizeof(char)]) = compressed_length;
  int written;
  int to_write_length = compressed_length + sizeof(char) + sizeof(int);
  int written_length = 0;
  do {
    int written = write(duplex->fd, &duplex->outgoing_compressed_buffer.data[written_length], to_write_length - written_length);
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      close(duplex->fd);
      duplex->fd = 0;
      break;
    } else if (written < 0)
      usleep(10000);
    else
      written_length += written;
  } while (written_length < to_write_length);
  return written_length;
}

static int recv_compressed_buffer(SDuplex* duplex) {
  if (!duplex->fd)
    return -1;
  int length = read(duplex->fd, &duplex->incoming_compressed_buffer.data[duplex->incoming_compressed_buffer.length], duplex->incoming_compressed_buffer.capacity - duplex->incoming_compressed_buffer.length);
  if (length < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
    close(duplex->fd);
    duplex->fd = 0;
    return 0;
  }
  if (length > 0)
    duplex->incoming_compressed_buffer.length += length;
  if (duplex->incoming_compressed_buffer.length < sizeof(char) + sizeof(int))
    return 1;
  int total_packet_length = array_reserve(&duplex->incoming_compressed_buffer, *((int*)&duplex->incoming_compressed_buffer.data[sizeof(char)]) + sizeof(char) + sizeof(int));
  if (duplex->incoming_compressed_buffer.length < total_packet_length)
    return 1;
  duplex->incoming_packet_type = *duplex->incoming_compressed_buffer.data;
  size_t decompressed_size = array_reserve(&duplex->incoming_buffer, ZSTD_getFrameContentSize(&duplex->incoming_compressed_buffer.data[sizeof(char) + sizeof(int)], total_packet_length - sizeof(char) - sizeof(int)));
  size_t decompressed_length = ZSTD_decompress(duplex->incoming_buffer.data, duplex->incoming_buffer.capacity, &duplex->incoming_compressed_buffer.data[sizeof(char) + sizeof(int)], total_packet_length - sizeof(char) - sizeof(int));
  if (ZSTD_isError(decompressed_length)) {
    fprintf(stderr, "Error: %d %s\n", decompressed_size, ZSTD_getErrorName(decompressed_length));
    close(duplex->fd);
    duplex->fd = 0;
    return 0;
  }
  duplex->incoming_buffer.length = decompressed_length;
  array_shift(&duplex->incoming_compressed_buffer, total_packet_length);
  return 1;
}

#define LUA_TINTEGER 200
static void push_lua(lua_State* L, int n, array_t* buffer) {
  array_append(buffer, &n, sizeof(n));
  for (int i = -n; i < 0; ++i) {
    int type = lua_type(L, i);
    if (type == LUA_TNUMBER && lua_isinteger(L, i))
      type = LUA_TINTEGER;
    array_append(buffer, &type, sizeof(type));
    switch (type) {
      case LUA_TNIL: break;
      case LUA_TSTRING: {
        size_t len;
        const char* str = lua_tolstring(L, i, &len);
        array_append(buffer, &len, sizeof(len));
        array_append(buffer, str, len);
      } break;
      case LUA_TINTEGER: {
        int n = lua_tointeger(L, i);
        array_append(buffer, &n, sizeof(n));
      } break;
      case LUA_TNUMBER: {
        double n = lua_tonumber(L, i);
        array_append(buffer, &n, sizeof(n));
      } break;
      case LUA_TBOOLEAN: {
        char c = lua_toboolean(L, i);
        array_append(buffer, &c, sizeof(c));
      } break;
    }
  }
  lua_pop(L, n);
}

static int pull_lua(lua_State* L, array_t* buffer) {
  int arg_count = 0;
  
  const char* cmd_ptr = buffer->data;
  const char* end = buffer->data + buffer->length;
  
  if (cmd_ptr < end) {
    arg_count = *((int*)cmd_ptr);
    cmd_ptr += sizeof(int);
    for (int i = 0; i < arg_count; ++i) {
      int type = *((int*)cmd_ptr);
      cmd_ptr += sizeof(int);
      switch (type) {
        case LUA_TNIL:
          lua_pushnil(L);
        break;
        case LUA_TINTEGER:
          lua_pushinteger(L, *(int*)cmd_ptr);
          cmd_ptr += sizeof(int);
        break;
        case LUA_TBOOLEAN:
          lua_pushboolean(L, *cmd_ptr);
          ++cmd_ptr;
        break;
        case LUA_TSTRING: {
          size_t length = *(size_t*)cmd_ptr;
          cmd_ptr += sizeof(size_t);
          lua_pushlstring(L, cmd_ptr, length);
          cmd_ptr += length;
        } break;
        case LUA_TNUMBER:
          lua_pushnumber(L, *(double*)cmd_ptr);
          cmd_ptr += sizeof(double);
        break;
      }
    }
  }
  return arg_count;
}

static int push_command(SRencache* rencache, Command* command) {
  array_append(&rencache->buffer, command, command->size);
  hash(&rencache->checksum, command, command->size);
  return command->size;
}

static int f_server_gc(lua_State* L) {
  SServer* server = lua_touserdata(L, 1);
  close(server->duplex.fd);
  free(server->duplex.incoming_buffer.data);
  free(server->duplex.incoming_compressed_buffer.data);
  free(server->duplex.outgoing_buffer.data);
  free(server->duplex.outgoing_compressed_buffer.data);
}

static int f_server_register_font(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  const char* path = luaL_checkstring(L, 2);
  const char* contents = luaL_checkstring(L, 3);
  struct RenFont* font = *(struct RenFont**)luaL_checkudata(L, 4, "Font");
  double size = lua_tonumber(L, 5);
  const char* options = luaL_optstring(L, 6, NULL);
  SFont sfont = (SFont){ server->registered_fonts.length + 1, font };
  array_append(&server->registered_fonts, &sfont, sizeof(SFont));
  lua_pushvalue(L, 2);
  lua_pushvalue(L, 3);
  lua_pushinteger(L, sfont.index);
  lua_pushvalue(L, 5);
  lua_pushvalue(L, 6);
  push_lua(L, 5, &server->duplex.outgoing_buffer);
  send_compressed_buffer(&server->duplex, PACKET_FONT_REGISTER, &server->duplex.outgoing_buffer);
  return 1;
}

static int f_server_begin_frame(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  array_clear(&server->rencache.buffer);
  server->rencache.checksum = HASH_INITIAL;
  return 0;
}

static int f_server_end_frame(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  if (server->rencache.checksum != server->previous_rencache_checksum && server->duplex.fd) {
    int flags = fcntl(server->duplex.fd, F_GETFL, 0);
    fcntl(server->duplex.fd, F_SETFL, flags & ~O_NONBLOCK);
    send_compressed_buffer(&server->duplex, PACKET_COMMAND_BUFFER, &server->rencache.buffer);
    fcntl(server->duplex.fd, F_SETFL, flags | O_NONBLOCK);
    server->previous_rencache_checksum = server->rencache.checksum;
    lua_pushboolean(L, 1);
  } else 
    lua_pushboolean(L, 0);
  return 1;
}


static int color_value_error(lua_State *L, int idx, int table_idx) {
  const char *type, *msg;
  // generate an appropriate error message
  if (luaL_getmetafield(L, -1, "__name") == LUA_TSTRING) {
    type = lua_tostring(L, -1); // metatable name
  } else if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
    type = "light userdata"; // special name for light userdata
  } else {
    type = lua_typename(L, lua_type(L, -1)); // default name
  }
  // the reason it went through so much hoops is to generate the correct error
  // message (with function name and proper index).
  msg = lua_pushfstring(L, "table[%d]: %s expected, got %s", table_idx, lua_typename(L, LUA_TNUMBER), type);
  return luaL_argerror(L, idx, msg);
}

static int get_color_value(lua_State *L, int idx, int table_idx) {
  lua_rawgeti(L, idx, table_idx);
  return lua_isnumber(L, -1) ? lua_tonumber(L, -1) : color_value_error(L, idx, table_idx);
}

static int get_color_value_opt(lua_State *L, int idx, int table_idx, int default_value) {
  lua_rawgeti(L, idx, table_idx);
  if (lua_isnoneornil(L, -1))
    return default_value;
  else if (lua_isnumber(L, -1))
    return lua_tonumber(L, -1);
  else
    return color_value_error(L, idx, table_idx);
}

static RenColor checkcolor(lua_State *L, int idx, int def) {
  RenColor color;
  if (lua_isnoneornil(L, idx)) {
    return (RenColor) { def, def, def, 255 };
  }
  luaL_checktype(L, idx, LUA_TTABLE);
  color.r = get_color_value(L, idx, 1);
  color.g = get_color_value(L, idx, 2);
  color.b = get_color_value(L, idx, 3);
  color.a = get_color_value_opt(L, idx, 4, 255);
  lua_pop(L, 4);
  return color;
}


static int f_server_set_clip_rect(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  lua_Number w = luaL_checknumber(L, 4);
  lua_Number h = luaL_checknumber(L, 5);
  SetClipCommand cmd = { { SET_CLIP, sizeof(SetClipCommand) }, rect_to_grid(x, y, w, h) };
  push_command(&server->rencache, &cmd.command);
  return 0;
}

static int f_server_draw_rect(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  lua_Number w = luaL_checknumber(L, 4);
  lua_Number h = luaL_checknumber(L, 5);
  DrawRectCommand cmd = { { DRAW_RECT, sizeof(DrawRectCommand) }, rect_to_grid(x, y, w, h), checkcolor(L, 6, 255) };
  push_command(&server->rencache, &cmd.command);
  return 0;
}

static int f_server_draw_text(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  if (server->duplex.fd) {
    int fonts[FONT_FALLBACK_MAX];
    if (lua_type(L, 1) != LUA_TTABLE) {
      struct RenFont* font = *(struct RenFont**)lua_touserdata(L, 2);
      fonts[0] = get_font_index(server, font);
      if (fonts[0] == -1)
        return luaL_error(L, "can't find unregistered font");
    } else {
      int len = luaL_len(L, 1); len = len > FONT_FALLBACK_MAX ? FONT_FALLBACK_MAX : len;
      for (int i = 0; i < len; i++) {
        lua_rawgeti(L, 1, i+1);
        struct RenFont* font = *(struct RenFont**)lua_touserdata(L, -1);
        fonts[i] = get_font_index(server, font);
        if (fonts[i] == -1)
          return luaL_error(L, "can't find unregistered font");
        lua_pop(L, 1);
      }
    }
    size_t len;
    const char *text = luaL_checklstring(L, 3, &len);
    double x = luaL_checknumber(L, 4);
    int y = luaL_checknumber(L, 5);
    RenColor color = checkcolor(L, 6, 255);
    static array_t cmd_array = {0};
    array_reserve(&cmd_array, sizeof(DrawTextCommand) + len);
    DrawTextCommand* cmd = (DrawTextCommand*)cmd_array.data;
    *cmd = (DrawTextCommand){ { .type = DRAW_TEXT, .size = sizeof(DrawTextCommand) + len }, .color = color, .fonts = *fonts, .text_x = x, .y = y, .len = len, .tab_size = 2 };
    memcpy(&cmd->text, text, len);
    push_command(&server->rencache, (Command*)cmd);
  }
  return 0;
}


static int f_server_accept(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  
  struct sockaddr_in peer_addr = {0};
  socklen_t peer_size = sizeof(peer_addr);
  server->duplex.fd = accept(server->listening, (struct sockaddr*)&peer_addr, &peer_size);
  if (server->duplex.fd == -1)
    return luaL_error(L, "can't accept: %s", strerror(errno));
  close(server->listening);
  server->listening = 0;
  int flags = fcntl(server->duplex.fd, F_GETFL, 0);
  fcntl(server->duplex.fd, F_SETFL, flags | O_NONBLOCK);
  lua_pushstring(L, inet_ntoa(peer_addr.sin_addr));
  return 1;
}


static int f_server_is_open(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  lua_pushboolean(L, server->duplex.fd != 0);
  return 1;
}


static int f_server_wait_event(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  if (server->duplex.fd) {
    lua_pushboolean(L, server->duplex.incoming_packet_type != PACKET_NONE || recv_compressed_buffer(&server->duplex));
    return 1; 
  }
  return 0;
}

static int f_server_poll_event(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  if (server->duplex.fd) {
    if (server->duplex.incoming_packet_type == PACKET_NONE)
      recv_compressed_buffer(&server->duplex);
    if (server->duplex.incoming_packet_type != PACKET_NONE) {
      int n = pull_lua(L, &server->duplex.incoming_buffer);
      array_clear(&server->duplex.incoming_buffer);
      server->duplex.incoming_packet_type = PACKET_NONE;
      return n;
    }
  }
  return 0;
}


static int f_server_send_event(lua_State* L) {
  SServer* server = luaL_checkudata(L, 1, "remoteserver");
  push_lua(L, lua_gettop(L) - 1, &server->duplex.outgoing_buffer);
  send_compressed_buffer(&server->duplex, PACKET_EVENT, &server->duplex.outgoing_buffer);
  return 0;
}


static const luaL_Reg server[] = {
  { "__gc",          f_server_gc            },
  { "begin_frame",   f_server_begin_frame   },
  { "end_frame",     f_server_end_frame     },
  { "set_clip_rect", f_server_set_clip_rect },
  { "draw_rect",     f_server_draw_rect     },
  { "draw_text",     f_server_draw_text     },
  { "register_font", f_server_register_font },
  { "accept",        f_server_accept        },
  { "wait_event",    f_server_wait_event    },
  { "poll_event",    f_server_poll_event    },
  { "send_event",    f_server_send_event    },
  { "is_open",       f_server_is_open       },
  { NULL,            NULL                   }
};

static int f_client_gc(lua_State* L) {
  SClient* client = lua_touserdata(L, 1);
  close(client->duplex.fd);
  free(client->duplex.incoming_buffer.data);
  free(client->duplex.incoming_compressed_buffer.data);
  free(client->duplex.outgoing_buffer.data);
  free(client->duplex.outgoing_compressed_buffer.data);
}

static int f_client_is_open(lua_State* L) {
  SClient* client = luaL_checkudata(L, 1, "remoteclient");
  lua_pushboolean(L, client->duplex.fd != 0);
  return 1;
}

static int f_client_send_event(lua_State* L) {
  SClient* client = luaL_checkudata(L, 1, "remoteclient");
  push_lua(L, lua_gettop(L) - 1, &client->duplex.outgoing_buffer);
  send_compressed_buffer(&client->duplex, PACKET_EVENT, &client->duplex.outgoing_buffer);
  return 0;
}

static void lua_pushcolor(lua_State* L, RenColor color) {
  lua_newtable(L);
  lua_pushinteger(L, color.r);
  lua_rawseti(L, -2, 1);
  lua_pushinteger(L, color.g);
  lua_rawseti(L, -2, 2);
  lua_pushinteger(L, color.b);
  lua_rawseti(L, -2, 3);
  lua_pushinteger(L, color.a);
  lua_rawseti(L, -2, 4);
}

static int f_client_has_event(lua_State* L) {
  SClient* client = luaL_checkudata(L, 1, "remoteclient");
  if (client->duplex.incoming_packet_type == PACKET_NONE && client->duplex.fd)
    recv_compressed_buffer(&client->duplex);
  lua_pushboolean(L, client->duplex.incoming_packet_type != PACKET_NONE);
  if (client->duplex.incoming_packet_type != PACKET_NONE) {
    FILE* file = fopen("/tmp/wat", "wb");
    fwrite(client->duplex.incoming_buffer.data, sizeof(char), client->duplex.incoming_buffer.length, file);
    fclose(file);
  }
  return 1;
}

static int f_client_process_event(lua_State* L) {
  SClient* client = luaL_checkudata(L, 1, "remoteclient");
  if (!client->duplex.fd) {
    lua_pushliteral(L, "quit");
    return 1;
  }
  luaL_checktype(L, 2, LUA_TFUNCTION); // renderer.set_clip
  luaL_checktype(L, 3, LUA_TFUNCTION); // renderer.draw_rect
  luaL_checktype(L, 4, LUA_TFUNCTION); // renderer.draw_text
  luaL_checktype(L, 5, LUA_TFUNCTION); // font_load(path, contents, options)
  if (client->duplex.incoming_packet_type == PACKET_NONE)
    return 0;
  int result_count = 0;
  array_t* result = &client->duplex.incoming_buffer;
  switch (client->duplex.incoming_packet_type) {
    case PACKET_COMMAND_BUFFER: {
      Command* command = (Command*)result->data;
      Command* end_command = (Command*)(result->data + result->length);
      while (command < end_command) {
        switch (command->type) {
          case SET_CLIP: {
            SetClipCommand* clip = (SetClipCommand*)command;
            lua_pushvalue(L, 2);
            lua_pushinteger(L, clip->rect.x);
            lua_pushinteger(L, clip->rect.y);
            lua_pushinteger(L, clip->rect.width);
            lua_pushinteger(L, clip->rect.height);
            lua_call(L, 4, 0);
          } break;
          case DRAW_RECT: {
            DrawRectCommand* rect = (DrawRectCommand*)command;
            lua_pushvalue(L, 3);
            lua_pushinteger(L, rect->rect.x);
            lua_pushinteger(L, rect->rect.y);
            lua_pushinteger(L, rect->rect.width);
            lua_pushinteger(L, rect->rect.height);
            lua_pushcolor(L, rect->color);
            lua_call(L, 5, 0);
          } break;
          case DRAW_TEXT: {
            DrawTextCommand* text = (DrawTextCommand*)command;
            lua_pushvalue(L, 4);
            lua_rawgeti(L, LUA_REGISTRYINDEX, client->font_table);
            lua_rawgeti(L, -1, text->fonts[0]);
            lua_replace(L, -2);
            if (!lua_isnil(L, -1)) {
              lua_pushlstring(L, text->text, text->len);
              lua_pushnumber(L, text->text_x);
              lua_pushnumber(L, text->y);
              lua_pushcolor(L, text->color);
              lua_call(L, 5, 0);
            } else {
              lua_pop(L, 2);
            }
          } break;
        }
        command = (Command*)(((char*)command) + command->size);
      }
    } break;
    case PACKET_FONT_REGISTER: {
      lua_rawgeti(L, LUA_REGISTRYINDEX, client->font_table);
      lua_pushvalue(L, 5);
      int n = pull_lua(L, result);
      int idx = lua_tointeger(L, -3);
      assert(n == 5); // path, contents, idx, size, options
      lua_call(L, 5, 1); // should return RenFont*
      lua_rawseti(L, -2, idx);
      lua_pop(L, 1);
    } break;
    case PACKET_EVENT:
      result_count = pull_lua(L, result);
    break;
  }
  client->duplex.incoming_packet_type = PACKET_NONE;
  return result_count;
}

static const luaL_Reg client[] = {
  { "__gc",              f_client_gc                  },
  { "send_event",        f_client_send_event          },
  { "process_event",     f_client_process_event       },
  { "has_event",         f_client_has_event           },
  { "is_open",           f_client_is_open             },
  { NULL,                NULL                         }
};


static int f_server(lua_State* L) {
  const char* hostname = luaL_optstring(L, 1, NULL);
  int port = luaL_checkinteger(L, 2);
  SServer* server = lua_newuserdata(L, sizeof(SServer));
  memset(server, 0, sizeof(SServer));
  luaL_setmetatable(L, "remoteserver");
  server->listening = socket(AF_INET, SOCK_STREAM, 0);
  int reuse = 1;
  setsockopt(server->listening, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  if (!server->listening)
    return luaL_error(L, "can't create socket: %s", strerror(errno));
  struct sockaddr_in host_addr = {0};
  host_addr.sin_addr.s_addr = hostname ? inet_addr(hostname) : INADDR_ANY;
  host_addr.sin_port = htons(port);
  host_addr.sin_family = AF_INET;  
  array_reserve(&server->duplex.incoming_compressed_buffer, 4096);
  array_reserve(&server->duplex.outgoing_compressed_buffer, 4096);
  if (bind(server->listening, (struct sockaddr*)&host_addr, sizeof(host_addr)) == -1)
    return luaL_error(L, "can't bind: %s", strerror(errno));
  if (listen(server->listening, 1) == -1)
    return luaL_error(L, "can't listen: %s", strerror(errno));
  return 1;
}


static int f_client(lua_State* L) {
  const char* hostname = luaL_optstring(L, 1, NULL);
  int port = luaL_checkinteger(L, 2);
  SClient* client = lua_newuserdata(L, sizeof(SClient));
  memset(client, 0, sizeof(SClient));
  luaL_setmetatable(L, "remoteclient");
  struct hostent *host = gethostbyname(hostname);
  if (!host)
    return luaL_error(L, "can't resolve host %s", hostname);
  struct sockaddr_in dest_addr = {0};
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(port);
  dest_addr.sin_addr.s_addr = *(long*)(host->h_addr);
  const char* ip = inet_ntoa(dest_addr.sin_addr);
  client->duplex.fd = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(client->duplex.fd, (struct sockaddr *) &dest_addr, sizeof(struct sockaddr)) == -1 ) {
    close(client->duplex.fd);
    client->duplex.fd = 0;
    return luaL_error(L, "can't connect to host %s [%s] on port %d", hostname, ip, port);
  }
  int flags = fcntl(client->duplex.fd, F_GETFL, 0);
  fcntl(client->duplex.fd, F_SETFL, flags | O_NONBLOCK);
  array_reserve(&client->duplex.incoming_compressed_buffer, 4096);
  array_reserve(&client->duplex.outgoing_compressed_buffer, 4096);
  lua_newtable(L);
  client->font_table = luaL_ref(L, LUA_REGISTRYINDEX);
  return 1;
}


static const luaL_Reg remote[] = {
  { "client",            f_client },
  { "server",            f_server },
  { NULL,                NULL }
};


#ifndef LIBREMOTE_STANDALONE
int luaopen_lite_xl_libremote(lua_State* L, void* XL) {
  lite_xl_plugin_init(XL);
#else
int luaopen_libtemote(lua_State* L) {
#endif
  luaL_newmetatable(L, "remoteclient");
  luaL_setfuncs(L, client, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  luaL_newmetatable(L, "remoteserver");
  luaL_setfuncs(L, server, 0);
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
  lua_newtable(L);
  luaL_setfuncs(L, remote, 0);
  return 1;
}
