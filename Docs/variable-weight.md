<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Font weight selection (SGR 1:n)

SGR 1 with a colon subparameter selects a run's font weight independently of
the ordinary bold flag.

## Wire format

```text
CSI 1 m       ordinary bold
CSI 1:<n> m   weight selector, n = 1..7
CSI 1:0 m     clear the weight selector
CSI 22 m      clear bold, dim, and the weight selector
CSI 0 m       reset the pen
```

Selectors 1–7 map to `wght` values 200–800 in steps of 100. The parser clamps
the selector to 0–7. A colon parameter is required: `CSI 1;3 m` means bold
and italic, not weight 3.

```sh
for n in 1 2 3 4 5 6 7; do printf "\\033[1:${n}mweight ${n}\\033[0m  "; done; echo
```

## Model and rendering

`SGR.cpp::applyWeightSelector` stores the selector through
`Pen::extendedAttributes.setWeight`. Bare SGR 1 sets `VT::Attribute::Bold`.
A nonzero selector takes precedence over the face's configured regular or bold
weight; it must not apply a second emboldening on top.

Core's `Rendering::Font::variationWeightForSGR` defines the numeric mapping.
`weightForFace` combines it with `font.weight` and `font.bold_weight`.
Weight participates in raster identity so differently weighted glyphs cannot
reuse the same atlas entry.

A variable face applies the selected value to its `wght` axis, within the
face's supported range. A static font cannot provide a continuous axis ramp:
the result depends on the backend's face-selection capability. The CoreText
catalogue's SGR-weight copy retains the regular face when it has no such axis;
the FreeType backend selects an available face and applies a variable axis
when present. Plain SGR 1 remains the ordinary bold request.

## References

- `Source/Core/SGR.cpp`: parser and reset semantics.
- `Source/Core/Rendering/Font/FontRoute.h`: shared weight policy.
- `Source/UbiquittyRenderKit/FontCatalogue.swift`: CoreText weight instances.
- `Source/Platform/Linux/FreeTypeRasterizer.cpp`: FreeType face selection.
