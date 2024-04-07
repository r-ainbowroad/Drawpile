# r/place Bronies headless drawpile client

This implements a headless drawpile client for making it easier to manage
templates for r/place like events.

## Command line

    Options:
      -?, -h, --help         Displays help on commandline options.
      --help-all             Displays help, including generic Qt options.
      -v, --version          Displays version information.
      -V, --verbose          Enable verbose output.
      -u, --url <url>        drawpile:// session URL.
      -o, --out <path>       Path to write output to.
      -t, --template <name>  Template name used for the topic name in mqtt and
                             folder on s3.
      -m, --mqtt <url>       URL for mqtt connection.
      -s, --s3 <url>         URL for s3 upload. Includes keys as username and
                             password.

--url, --out, --template, --mqtt, and --s3 are required.

## How it works

The client connects to the provided drawpile session and listens to drawpile
messages and waits for chat commands. When a user commits a change, it takes
the canvas state at the exact moment of the commit message and uses that to
update the template.

### Chat Commands
<dl>
  <dt><code>!commit [commit-message]</code></dt>
  <dd>Updates the template to match the current drawpile canvas. The client will
      respond with <code>%committed [commit-id]</code> and then some details
      about the update.</dd>
  <dt><code>!reset</code></dt>
  <dd>Resets the internal change tracking state of the of client. This is only
      used for recovering from bugs in template delta tracking.</dd>
</dl>

### Responses

TODO: Document the extra messages that the client might send and why it sends
them (it uses drawpile chat for state tracking on reconnect).

### Commits

TODO: Document in more detail what it actually means to do a commit. There's
palettization, diffing, cropping, endu style template output, etc.

### Layers

TODO: Document layer name format and how layer rendering is different from 
drawpile's normal blending.

## Building

You will additionally need [rclone](https://rclone.org/) available in your path.

### Windows

See https://docs.drawpile.net/help/development/buildingfromsource#windows, with
the following additions:

You need Qt6 instead of Qt5, so vcpkg should be:

    vcpkg --disable-metrics install --clean-after-build qtbase qtmultimedia qtsvg  qttools qttranslations qtwebsockets kf5archive libmicrohttpd libsodium qtkeychain qtmqtt qcoro

This additionally adds QtMqtt and QCoro as dependencies.

Use the `windows-x64-{debug,release}-qt6-client-ninja` presets

You may need to copy the tls folder from
`vcpkg\buildtrees\qtbase\x64-windows-{dbg,rel}\Qt6\plugins` into your drawpile
bin folder.

## Protocol

TODO: Document the mqtt protocol used.
