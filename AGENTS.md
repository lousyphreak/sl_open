# sl_open

sl_open reimplements the original StarLancer executable. Base gameplay code
closely on the original EXE and DLLs; documentation only helps locate evidence
in the binaries and is not authoritative. Do not approximate behavior.
Original binaries and game data are kept outside this repository.

- Build with 24 parallel jobs by default.
- Do not add tests or defensive checks.
- Do not run the game or commit changes unless specifically asked.
- When fixing a bug, inspect nearby code for related issues.
- Use direct structs and functions. Avoid speculative abstractions, duplicate
  state, redundant checks, and unnecessary allocations.
- Keep steady-state gameplay, rendering, and audio updates allocation-free.
- Preserve retail visuals while using GPU work, batching, and full meshes.
  Build render data at load time rather than rebuilding triangles each frame.
