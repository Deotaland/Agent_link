# Dynamic OTA slots

Sugarat Button uses physical layout `sdgoods-v4` with a 10 MiB application pool.
Persistent NVS/font/appdata addresses remain those of V3. The application ABI
remains 3: native V3 apps still recognize the Factory descriptor and can return
to it. The public name is Agent_link Launcher; IDF project_name remains
SDGOODS_LAUNCHER for unmodified vendor applications.

The CSV is a bootstrap table with one pool-sized ota_0. On the first Factory
boot it becomes 16 contiguous OTA subtype records. Empty slots have size zero
at the pool end; allocated slots occupy 64 KiB-aligned, nonoverlapping extents.
Firmware plus its 56 KiB cover reserve determines the required extent. The
allocator uses the smallest sufficient free extent; it never moves live apps.
Deleting a slot releases its extent. Fragmentation can reject an image even
when aggregate free space would suffice; automatic compaction is not performed.

New native images append an optional capability record after the existing
176-byte descriptor. Legacy native V3 images remain restricted to ota_0/ota_1
because their Ready() implementation only accepts those subtypes. New native
images and compatible vendor images can use all 16 indices. Image bytes are
never patched to change identity or capabilities.

Table changes require the matching bootloader hook. A two-bank journal at
pt_guard stores complete tables, sequence numbers, CRCs and a final commit word.
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
