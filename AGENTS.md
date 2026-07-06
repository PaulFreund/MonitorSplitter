# Agent Notes

## DisplayCore Testing

If testing hits `DXGI_ERROR_MODE_CHANGE_IN_PROGRESS` (`0x887a0025`), stop the test sequence and ask the user to reboot before continuing. Do not reboot the machine yourself. Treat this as a stuck system state rather than a recoverable transient.

When this error appears, capture what happened immediately before it: the command sequence, whether the panel was in native or splitter mode, recent hibernate/resume/logon/display-change events, and the latest MonitorSplitter service and host status/log output. The priority is to understand what transition led Windows/DisplayCore into the stuck mode-change state.
