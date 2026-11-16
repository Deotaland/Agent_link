# Dynamic OTA slots

Sugarat Button uses physical layout `sdgoods-v5` with a 10 MiB application pool.
Application data retains its start address and has 13 MiB; configuration/font
addresses move in this initial USB layout migration. The application ABI
remains 3; native capability version 2 identifies support for the V5 names. The public name is Sugarat Launcher; IDF project_name remains
SDGOODS_LAUNCHER for unmodified vendor applications.

The CSV is a bootstrap table with one pool-sized `app_pool`. On the first Factory
boot it becomes 16 contiguous OTA subtype records. User labels are app_00..app_13;
launcher_a/launcher_b keep subtypes ota_14/ota_15. Indices 14/15 are fixed,
reserved A/B Launchers and never enter allocation/removal. Empty user slots have size zero
at the pool end; allocated slots occupy 64 KiB-aligned, nonoverlapping extents.
Firmware plus its 56 KiB cover reserve determines the required extent. The
allocator uses the smallest sufficient free extent; it never moves live apps.
Deleting a slot releases its extent. Fragmentation can reject an image even
when aggregate free space would suffice; automatic compaction is not performed.

New native images append an optional capability record after the existing
176-byte descriptor. Named V5 requires capability version 2 for native images, since older native
SDKs hard-code app_nvs/ota_N labels. They must be rebuilt. Original vendor
applications retain their boot subtypes and appdata label unchanged. New native
images and compatible vendor images can use the 14 user indices. Image bytes are
never patched to change identity or capabilities.

Table changes require the matching bootloader hook. A two-bank journal at
`layout_journal` stores complete tables, sequence numbers, CRCs and a final commit word.
The new table is fully validated and journaled before erasing the primary table.
Before reading the primary table, the bootloader restores the newest committed
valid table if required. The fixed Factory, persistent partitions and pool bounds
are checked before any restore. Interrupted writes leave the old or new committed
layout usable. Full USB packages reset the journal; app-only USB flashing is
disabled on dynamic layouts. Existing V3 bootloaders cannot authorize table writes.

Installation writes and verifies a candidate in a free pool extent before
publishing the table. A successful geometry change reboots into Factory to reload
IDF's partition cache; only then can an application be launched. Runtime partition
updates never rewrite Factory, credentials, font or appdata. This recovery protocol
does not make an interrupted USB full flash or arbitrary third-party erase safe.

Validation must cover actual package SHA/image checks, more than two installed
apps, both native capability versions, allocation/reuse/full/fragmented pools,
and power loss at journal and primary-table write boundaries. Device acceptance
still requires representative vendor apps, normal return and physical recovery.
