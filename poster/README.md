# MycoFlow A1 Poster

This folder contains a self-contained A1 landscape poster draft for the MycoFlow project.

## Files

- `mycoflow_a1_poster.tex` — printable poster source

## Build

From this directory, run:

```bash
latexmk -pdf -interaction=nonstopmode mycoflow_a1_poster.tex
```

If `latexmk` is unavailable, use `pdflatex` twice:

```bash
pdflatex mycoflow_a1_poster.tex
pdflatex mycoflow_a1_poster.tex
```

## Notes

- The poster is written in English to match the project paper and IEEE-style technical framing.
- The QR/scan box is intentionally left as a placeholder so you can add a link to the demo, paper, or repository later.
- If you want a Turkish version, the content can be translated without changing the layout.