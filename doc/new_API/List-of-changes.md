# List of changes in API this draft is trying to present

- Removal of "frame" mode and rtp "mode", modified pipeline mode stays as only API for MTL sessions

- Polymorphic session classes used to reduce code repetition

- Replacing callback with event polling function to notify app about events in lib

- Change of usage pattern for zero copy/external frame mode. A clear separation of two types of
buffer ownership; buffers owned by the library and buffers owned by the application.

- Added stop() start() and shutdown()

> **Note**
> Fields in all structures are dummy, they need to be correctly ported from current API. Also a few functions need to ported.
> This is not a complete API, just a presentation of a few ideas.