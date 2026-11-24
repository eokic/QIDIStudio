# QIDI Studio printers

The words for the printers that QIDI Studio and its MCP server talk to, and for the lists that hold them.

## Language

**LAN printer**:
A real printer that QIDI Studio talks to directly over the local network, with no QIDI cloud in between. QIDI Studio knows each LAN printer as a physical printer with a host address.
_Avoid_: printer host, Moonraker printer, network printer

**Physical printer**:
QIDI Studio's saved entry for one real printer: a name, a host address, an API key, and the printer model it was set up with. The MCP printer tools use this list.
_Avoid_: printer preset, device

**Printer preset**:
A slicing profile for one printer model. Many physical printers can use the same printer preset.
_Avoid_: printer, physical printer

**Device**:
A printer in the Device tab's own list, which the Device tab keeps apart from the physical printers.
_Avoid_: physical printer, LAN printer
