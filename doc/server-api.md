Server admin REST API
---------------------

The standalone server provides a RESTful JSON API for administration.
The API is activated with the `--web-admin-port` command line parameter.
For security, it is important that the API is not publicly accessible!
If public access is needed (e.g. to list available sessions,) create a
web application that communicates with the server and presents the data.

The body of POST and PUT messages should be a JSON document with the content type `application/json`.

## Server status

`GET /api/status/`

Returns server status information:

    {
        "started": "yyyy-mm-dd hh:mm:ss"  (server startup timestamp UTC+0)
        "sessions": integer               (number of active sessions)
        "maxSessions": integer            (max active sessions)
        "users": integer                  (number of active users)
        "ext_host": "hostname"            (server's hostname, as used in session listings)
        "ext_port": integer               (server's port, as used in session listings)
    }


## Serverwide settings

`GET /api/server/`

Returns a list of server settings:

    {
        "clientTimeout": seconds     (connection timeout)
        "sessionSizeLimit": bytes    (maximum session size, or 0 for unlimited)
        "sessionCountLimit": integer (maximum number of active sessions)
        "persistence": boolean       (allow sessions to persist without users)
        "allowGuestHosts": boolean   (allow users without the HOST privilege to host sessions)
        "idleTimeLimit": seconds     (delete session after it has idled for this long)
                                     (0 means no time limit)
        "serverTitle": "string"      (title to be shown in the login box)
        "welcomeMessage": "string")  (welcome chat message sent to new users)
        "privateUserList": boolean   (if true, user list is never included in announcements)
        "allowGuests": boolean       (allow unauthenticated logins)
        "archive": boolean           (archive file backed sessions instead of deleting them)
        "extauth": boolean           (enable external authentication)
                                     (auth server URL must have been set via command line parameter)
        "extauthkey": "key string"   (ext-auth server public key)
        "extauthgroup": "group id"   (user group id. Leave blank to use default group)
        "extauthfallback": boolean   (fall back to guest logins if ext-auth server is not reachable)
        "extauthmod": boolean        (respect ext-auth user's MOD flag)
        "reporttoken": "token"       (authorization token for abuse report server)
        "logpurgedays": integer      (log entries older than this many days are automatically purged)
                                     (0 means logs are not purged)
        "autoResetThreshold": bytes  (session size at which autoreset request is sent)
                                     (Should be less than sessionSizeLimit. Can be overridden per-session)
        "customAvatars": boolean     (allow use of custom avatars. Custom avatars override ext-auth avatars)
        "extAuthAvatars": boolean    (allow use of ext-auth avatars)
    }

To change any of these settings, send a `PUT` request. Settings not
included in the request are not changed.

Values that accept seconds also accept time strings like "1.5 d"
Values that accept bytes also accept file sizes like "15 mb"

Implementation: `serverJsonApi @ src/server/multiserver.cpp`

See also `src/srver/serverconfig.h` for the most up to date list of supported settings.


## Sessions

Get a list of active sessions: `GET /api/sessions/`

Returns:

    [
        {
            "id": "uuid"             (unique session ID)
            "alias": "alias"         (ID alias, if set)
            "protocol": "xx:1.2.3"   (protocol version)
            "userCount": integer     (number of users)
            "maxUserCount": integer  (maximum number of users)
            "founder": "username"    (name of the user who created the session)
            "title": "string"        (session title)
            "persistent": boolean    (is persistence activated. Only included if feature is enabled serverwide)
            "hasPassword": boolean   (is the session password protected)
            "closed": boolean        (is the session closed to new users)
            "authOnly": boolean      (are only registered users allowed)
            "nsfm": boolean          (does this session contain age restricted content)
            "startTime": "timestamp" ()
            "size": bytes            (session history size)
        }, ...
    ]

Implementation: `callSessionJsonApi @ src/libserver/sessionserver.cpp`

Get detailed information about a session: `GET /api/sessions/:id/`

    {
        *same fields as above
        "maxSize": bytes        (maximum allowed size of the session)
        "resetThreshold": bytes (autoreset threshold)
        "deputies": boolean     (are trusted users allowed to kick non-trusted users)
        "hasOpword": boolean    (is an operator password set)
        "users": [
            {
                "id": integer       (user ID. Unique only within the session)
                "name": "user name"
                "ip": "IP address"
                "auth": boolean     (is authenticated),
                "op": boolean       (is a session owner),
                "muted": boolean    (is blocked from chat),
                "mod": boolean      (is a moderator),
                "tls": boolean      (is using a secure connection),
                "online": boolean   (if false, this user is no longer logged in)
            }, ...
        ],
        "listings": [
            {
                "id": integer           (listing entry ID number)
                "url": "url"            (list server address)
            }, ...
        ]
    }

Updating session properties: `PUT /api/sessions/:id/`

The following properties can be changed:

    {
        "closed": true/false,
        "authOnly": true/false,
        "persistent": true/false (only when persistence is enabled serverwide),
        "title": "new title",
        "maxUserCount": user count (does not affect existing users),
        "resetThreshold": size in bytes, or 0 to disable autoreset,
        "password": "new password",
        "opword": "new operator password",
        "preserveChat": true/false (include chat in history),
        "nsfm": true/false,
        "deputies": true/false
    }

To send a message to all session participants: `PUT /api/sessions/:id/`

    {
        "message": "send a message to all participants",
        "alert": "send an alert to all participants"
    }

To shut down a session: `DELETE /api/sessions/:id/`

Implementation: `callJsonApi @ src/shared/server/session.cpp`

### Session users

Kick a user from a session: `DELETE /api/sessions/:sessionid/:userId/`

Change user properties: `PUT /api/sessions/:sessionid/:userId/`

    {
        "op": true/false (op/deop the user)
    }

To send a message to an individual user: `PUT /api/sessions/:sessionid/:userId/`

    {
        "message": "message text"
    }

Implementation: `callUserJsonApi @ src/shared/server/session.cpp`

## Logged in users

`GET /api/users/`

Returns a list of logged in users:

    [
        {
            "session": "session ID" (if empty, this user hasn't joined any session yet)
            "id": integer           (unique only within the session),
            "name": "user name",
            "ip": "IP address",
            "auth": boolean         (is authenticated),
            "op": boolean           (is session owner),
            "muted": boolean        (is blocked from chat),
            "mod": boolean          (is a moderator),
            "tls": boolean          (is using a secure connection)
        }
    ]

Implementation: `callUserJsonApi @ src/shared/server/sessionserver.cpp`

## User accounts

`GET /api/accounts/`

Returns a list of registered user accounts:

    [
        {
            "id": integer              (internal account ID number)
            "username": "username"
            "locked": boolean          (is this account locked)
            "flags": "flag1,flag2"     (comma separated list of flags)
        }, ...
    ]

Possible user flags are:

 * HOST - this user can host new sessions (useful when allowGuestHosts is set to false)
 * MOD - this user is a moderator (has permanent OP status, may enter locked sessions)

To add a new user: `POST /api/accounts/`

    {
        "username": "username to register",
        "password": "user's password",
        "locked": true/false,
        "flags": ""
    }

To edit an user: `PUT /api/accounts/:id/`

    {
        "username": "change username",
        "password": "change password",
        "locked": change lock status,
        "flags": "change flags"
    }

To delete a user: `DELETE /api/accounts/:id/`

Implementation: `accountsJsonApi @ src/server/multiserver.cpp`

## Banlist

`GET /api/banlist/`

Returns a list of serverwide IP bans:

    [
        {
            "id": ban entry ID number,
            "ip": "banned IP address",
            "subnet": "subnet mask (0 means no mask, just the individual address is banned)",
            "expires": "ban expiration date",
            "comment": "Freeform comment about the ban",
            "added": "date when the ban was added"
        }
    ]

To add a ban, make a `POST` request to `/api/banlist/`:

    {
        "ip": "IP to ban",
        "subnet: "Subnet mask (0 to ban just the single address)",
        "expires": "expiration date",
        "comment": "freeform comment"
    }

To delete a ban, make a `DELETE /api/banlist/:id/` request.

Implementation: `banlistJsonApi @ src/server/multiserver.cpp`

## List server whitelist

`GET /api/listserverwhitelist/`

Returns:

    {
        "enabled": boolean
        "whitelist": [
            "regexp",
            ...
        ]
    }

If `enabled` is false, the whitelist is not applied and any list server
can be used. To block all list servers, set `enabled` to true and leave the
whitelist array empty.

The `whitelist` array is a list of regular expressions that match list server
URLs. (For example: `^https?://drawpile.net/api/listserver`)

To change the whitelist, make a PUT request with a body in the same format
as returned by the GET request.

Implementation: `listserverWhitelistJsonApi @ src/server/multiserver.cpp`


## Server log

`GET /api/log/`

The following query parameters can be used to filter the result set:

 * ?page=0/1/2/...: show this page
 * ?session=id: show messages related to this session
 * ?after=timestamp: show messages after this timestamp

Returns:

    [
        {
            "timestamp": "log entry timestamp (UTC+0)",
            "level": "log level: Error/Warn/Info/Debug",
            "topic": "what this entry is about",
            "session": "session ID (if related to a session)",
            "user": "ID;IP;Username triple (if related to a user)",
            "message": "log message"
        }, ...
    ]

Possible topics are:

 * Join: user join event
 * Leave: user leave event
 * Kick: this user was kicked from the session
 * Ban: this user was banned from the session
 * Unban: this user's ban was lifted
 * Op: this user was granted session ownership
 * Deop: this user's session owner status was removed
 * Mute: this user was blocked from chat
 * Unmute: this user can chat again
 * BadData: this user sent an invalid message
 * RuleBreak: this committed a protocol violation
 * PubList: session announcement related messages
 * Status: general status messages

Implementation: `logJsonApi @ src/server/multiserver.cpp`
