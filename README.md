# PocketVT

PocketVT is a highly optimized, low-level System-on-a-Chip (SoC) translation layer designed to run unreleased budget famiclone and commercial VT03/VT09 hardware software natively on the Game Boy Advance (ARM7TDMI). It has been made with the assistance of AI over the course of 2 months.

By decoupling the architecture from platform-locked Win32 desktop frameworks and squeezing the guest system execution loop directly into the strict real-time memory boundaries of a 16.78 MHz handheld processor, PocketVT establishes a definitive baseline for physical hardware constraints and low-level preservation performance.

* Verified Core: Core 6502 processing, opcode fetches, and unaligned memory mappings are 100% physically stable.
* Overworld Fixed: Sprite tile configurations and background nametables render with zero artifacts or left-path boundary distortions.
* Audio Active: Music engine and system-level audio pacing are dynamically tracking real-time frame updates.

* **Please note:** Only Lonely Island.nes has been tested to work fully. Other games still need to be added!

## Compile

sudo docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm make

## Credits

PocketVT is a preservation project built upon decades of collective community reverse-engineering. This software is proudly standing on the shoulders of giants:

* PocketNES (Loopy, Flubba, Dwedit): The spiritual blueprint and foundational technical inspiration for performance-driven NES emulation layout on the Game Boy Advance. PocketVT utilizes legacy memory-squeezing paradigms pioneered by the PocketNES development team to manipulate the ARM7TDMI memory footprint cleanly.

* Nintendulator-NRS (NewRisingSun): Universal gratitude is extended to NewRisingSun for their invaluable, industry-standard Windows implementation of the VT03 architecture. Their deep, surgical reverse-engineering documentation of niche famiclone hardware registers, mapper xml specifications, and scrambled opcode tables served as the definitive behavioral truth for verifying our state machine logic.

* MAME (The MAME Team): Acknowledgment to the MAME development community for their relentless dedication to legacy hardware documentation and structural chipset preservation, including VT03.

* NESdev community: The factual anchor of this entire project. Without the collective research, hardware register diagrams, and documentation hosted by the NESdev Wiki community, mapping out this territory would have been completely impossible.

* Claude Opus 4.8 and Fable 5 for allowing this all to be reverse-engineered.

* Gemini for helping generate the documentation.

## See also

https://github.com/GamingFanboy365/pocketnes , a project where I'm adding NES 2.0 mappers to PocketNES in addition to existing mappers.
