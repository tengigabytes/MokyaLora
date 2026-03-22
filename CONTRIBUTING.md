# Contributing to MokyaLora

Thank you for your interest in contributing to MokyaLora!

## How to Contribute

### Reporting Issues

- Use GitHub Issues to report bugs or request features.
- Include hardware revision, test conditions, and measurement data where applicable.

### Submitting Changes

1. Fork the repository and create a feature branch.
2. Make your changes with clear commit messages.
3. Open a Pull Request against `main` with a description of what changed and why.

### Hardware Changes

- Update the KiCad schematic and PCB files under `hardware/kicad/`.
- Export updated fabrication outputs to `hardware/production/rev-<X>/`.
- Document design decisions in `docs/design-notes/`.

### Firmware Changes

Please respect the license boundary between firmware components:

| Directory              | License      | Rule                                                         |
|------------------------|--------------|--------------------------------------------------------------|
| `firmware/core0/`      | GPL-3.0      | May include Meshtastic headers. Must not include Core 1 code.|
| `firmware/core1/`      | Apache-2.0   | Must NOT include any Meshtastic or Core 0 header.           |
| `firmware/mie/`        | MIT          | Must NOT include any Meshtastic, Core 0, or Core 1 header.  |
| `firmware/shared/ipc/` | MIT          | Only shared interface between Core 0 and Core 1.            |

The golden rule: **Core 1 and MIE may only cross the Core 0 boundary via `ipc_protocol.h`.**
Any pull request that adds a Meshtastic `#include` into `firmware/core1/` or `firmware/mie/`
will be rejected.

### Documentation Changes

- Documentation is licensed under CC-BY-SA-4.0.
- Keep file paths and technical terms in English.

## License Summary

See `LICENSE` for the full component-level license table. By contributing to this project
you agree that your contributions will be licensed under the same license as the component
you are modifying.

## AI-Assisted Development

This project uses AI coding tools (Claude Code) for firmware and tooling development.
AI-generated contributions are reviewed, tested, and accepted by the project maintainers
before being committed. All such contributions are subject to the same license terms as
the directory they reside in.

## Code of Conduct

Be respectful and constructive. This project is maintained by volunteers.
