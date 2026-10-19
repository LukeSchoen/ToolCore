ToolCore is a small, deliberately limited dev tool that does one thing: locally shares a single mouse and keyboard.

The Tool(s) app is a Windows client written in C. It keeps an authenticated connection to a ToolCore server.

To use Tool(s), you need to be physically present: Tool cannot capture, display, or send screen content:
so someone has to be present and there next to a computer to see the screen anyway.

Tool does a little bit of networking and encryption, and some basic input-direction and handling.

It's meant to useful while being minimal and harmless rather than powerful.

ToolCore includes no GUI and no Server.

Details / How it works:
Asks for: screen resolution, display brightness, volume and curr-build identification
It sends: beeps, toast messages, shutdown/restart requests, mouse, wheel and keyboard

Build
From this directory:

bat
build_cpc.c

Run and configure

Place a matching `token.txt` beside `bin\tool.exe`. Before starting the
endpoint, create `servers.txt` in the same folder with at least one server:

```text
server.example 7901
```

`--server <host>` and `TOOL_SERVER` name one server directly.
`--port <n>` and `TOOL_PORT` change its port and must be used with a server.
There is no built-in server address.
