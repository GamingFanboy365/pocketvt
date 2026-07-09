# PocketVT

PocketVT is a highly optimized, low-level System-on-a-Chip (SoC) translation layer designed to run unreleased budget famiclone and commercial VT03/VT09 hardware software natively on the Game Boy Advance (ARM7TDMI). 

By decoupling the architecture from platform-locked Win32 desktop frameworks and squeezing the guest system execution loop directly into the strict real-time memory boundaries of a 16.78 MHz handheld processor, PocketVT establishes a definitive baseline for physical hardware constraints and low-level preservation performance.

## 🚀 Current Status: 60 FPS Milestone Reached!
PocketVT has officially shattered previous performance deadlocks, jumping from preliminary frame-throttled states straight to a locked, full-speed **60 FPS** execution profile on real hardware. 

* **Verified Core:** Core 6502 processing, opcode fetches, and unaligned memory mappings are 100% physically stable.
* **Overworld Fixed:** Sprite tile configurations and background nametables render with zero artifacts or left-path boundary distortions.
* **Audio Active:** Music engine and system-level audio pacing are dynamically tracking real-time frame updates.

*Current Development Frontier:* Resolving horizontal camera scroll offset register calculations during screen transitions in active gameplay scenes.

## ⚙️ Architecture & Constrained Invariants
To achieve bare-metal efficiency on a cycle-starved ARM processor, PocketVT enforces zero-tolerance hardware design constraints:
* **Stackless Read Pipeline:** Memory read handlers (`readmem_*` hooks) utilize dedicated register pools and run with absolute zero ARM stack access, concluding exclusively with clean tail-jumps to `IO_R`.
* **The IWRAM Boundary:** To avoid silent memory mirroring and critical user stack corruption, the engine profile tightly targets an IWRAM ceiling (`__bss_end__`) below `0x03007B00`.
* **Optimized Palette Caching:** Fast-path rendering relies on deterministic palette dirty-flag management, avoiding redundant `vt_chr4_dirty` calls on standard background writes to preserve the 60 FPS video loop.

## 📦 Local Build Environment
To compile the production binary, ensure your localized cross-compilation toolchain is configured:

## Credits

PocketVT is a preservation project built upon decades of collective community reverse-engineering. This software is proudly standing on the shoulders of giants:

* PocketNES (Loopy, Flubba, Dwedit): The spiritual blueprint and foundational technical inspiration for performance-driven NES emulation layout on the Game Boy Advance. PocketVT utilizes legacy memory-squeezing paradigms pioneered by the PocketNES development team to manipulate the ARM7TDMI memory footprint cleanly.

* Nintendulator-NRS (NewRisingSun): Universal gratitude is extended to NewRisingSun for their invaluable, industry-standard Windows implementation of the VT03 architecture. Their deep, surgical reverse-engineering documentation of niche famiclone hardware registers, mapper xml specifications, and scrambled opcode tables served as the definitive behavioral truth for verifying our state machine logic.

* MAME (The MAME Team): Acknowledgment to the MAME development community for their relentless dedication to legacy hardware documentation and structural chipset preservation, including

* NESdev Community: The factual anchor of this entire project. Without the collective research, hardware register diagrams, and documentation hosted by the NESdev Wiki community, mapping out this territory would have been completely impossible.

```bash
# Execute the high-fidelity cross-compilation script
./build_pvt.sh
