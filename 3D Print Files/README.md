# 3D Print Files

This folder contains the printable parts specific to the XTE-DCDU build.

## Contents

- **`CDCU_Main_Panel.stl`** — front frame / bezel for the DCDU, modified to
  fit the **Guition JC3248W535C** all-in-one ESP32-S3 + 3.5" 320×480
  capacitive touch display used by this project, and to accommodate the
  eight push-buttons wired to the board's P2 header.

## Attribution

The front frame is a **modification of James Bennet's original DCDU front
frame design**, shared in the *A320 Cockpit Builders* Facebook group.
All credit for the original geometry, fit and finish goes to James — this
fork only adjusts the display cut-out, button positions and mounting bosses
for the JC3248W535C.

- Original post (parts list, full STL set, build notes):
  <https://www.facebook.com/groups/817102292436795/permalink/2156310058516005/>

If you are building a full A320 DCDU module, you will want the rest of
James' parts (rear shell, button caps, light plate, etc.) from that
original post — only the modified front frame is redistributed here.

Please respect James' original sharing terms on the Facebook post if you
remix or redistribute the surrounding parts.

## Printing notes

- Supports: only required under the mounting bosses on the rear face.
- Layer height: 0.2 mm is sufficient; 0.12 mm if you want crisper bezel
  edges.
- Infill: 25–35% gyroid or grid.
