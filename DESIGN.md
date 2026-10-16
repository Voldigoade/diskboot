# DiskBoot interface

Preserved design intent for a discontinued prototype. Visual, accessibility, DPI and complete workflow validation remain incomplete; see [STATUS.md](STATUS.md).

Native Windows product interface for a user selecting an ISO and a secondary SSD. Task clarity and accurate safety status take priority over decoration. The signature is the persistent distinction between protected Windows disks and the chosen Linux allocation.

The runtime source is `ui/main.cpp`: `Theme::margin` (24 logical pixels), body text (16) and heading (24). Segoe UI normal and semibold follow Windows conventions. Native system colors, selection, focus, disabled controls, file picker and confirmation dialogs own their visual states and respect user settings. There are no custom color tokens or animations.

The window has a minimum usable size of 740 by 600 logical pixels and scales to the current monitor DPI. Content scrolls within the details panel. Native controls provide keyboard navigation and accessibility semantics. The supported language is English; all paths use Unicode Win32 APIs.

ISO inspection and planning run on a worker, disable conflicting actions and preserve inputs on failure. Protected disk selection is refused immediately. Review uses the shared CLI planner. Physical preparation stays unavailable until a boot profile is validated. Laboratory preparation requires the explicit virtual disk file and one target-specific confirmation. An unverified boot path is never presented as ready to install.
