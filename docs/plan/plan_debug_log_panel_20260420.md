# Debug Log Panel Plan

1. Add a dedicated GUI log panel that stores recent messages and renders a dockable debug log window.
2. Route AppWindow status updates through a single helper so status text and debug log stay in sync.
3. Integrate the log panel into the main UI, including menu actions such as clearing the log and removing the old bottom status text-only presentation.
4. Build `//src:jtag_viewer` to verify the new panel compiles cleanly.