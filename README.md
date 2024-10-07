## Remote

This is an remote execution plugin for lite-xl, using libssh.

Basically, this plugin has two modes. Server, and client.

When set to client, this will connect to the specified port, and route everything coming in from `system.poll_event` to the remote end, using `common.serialize`.
Receives back all renderer calls. The amount of network traffic here is probably negligible, but can be compressed with zstd anyway.

When set to server, this will open up a port, and run completely as normal, except all `renderer` calls, and `renwindow` will be overriden,
and passed back to the client. Seemingly, the rencache buffer for a normal frame with a bunch of text is 5k zipped with zstd. This is 300k/s for 60 frames/s.
Which is doable. A slight modification of this would be to return with `end_frame` a boolean as to whether or not the frame changed at all from the previous
frame. If it didn't, we can omit the frame. This would save significant space with very little effort and would probably reduce the network load significantly.

If we only send frames, normal editing seems to be about 50k/s. This is well within the realm of possibility for immediate use by most people on hardline connections.
Which presumably you'd be on.

If this proves to be too heavy, we will send only the resulting rencache drawcalls when a frame ends. This would probably cut down the frame size significantly, but would
require core modifications or a pluggable renderer.

Threshold of noticing things is probably around 100ms. So a server probably needs a 50ms latency to be safe for this approach to be viable.


If we modified core rencache, I wouldn't even need to duplicate rencache on our side, I'd just package up the draw calls, and unpack them.


