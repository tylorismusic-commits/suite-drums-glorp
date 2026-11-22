# Security Policy

## Reporting a vulnerability

Please do not open a public issue for a security problem. Use GitHub's private
vulnerability reporting for
`tylorismusic-commits/suite-drums-glorp`, or contact the maintainer through the
repository.

Include the affected version, plugin format, host, and a minimal reproduction.
You can expect an acknowledgement within a few days.

## Scope

Suite Drums Glorp is an audio plugin and a desktop kit editor.

- No network access. The plugin does not make network connections.
- Kit files are parsed locally. Malformed `.glorpkit`, `.zip` and DrumGizmo XML
  files are handled defensively; report crashes or memory errors.
- User data lives under the platform application-data directory:
  - `~/.config/SuiteDrumsGlorp/settings.txt`
  - `~/.config/SuiteDrumsGlorp/presets/`
  - `~/.config/SuiteDrumsGlorp/kit_layout.json`
  - `~/.config/SuiteDrumsGlorp/suitedrumsglorp_debug.log`
  - `~/.config/SuiteDrumsGlorp/kits/`
- Legacy `~/.config/GlorpDrums/` data is read or migrated on first run.

## Supported versions

Only the latest alpha release is supported while the project is in early
development.
