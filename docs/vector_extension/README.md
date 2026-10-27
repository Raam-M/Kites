# Kites Vector Extension: Project Documents

Planning, design and validation documents for adding the RISC-V Vector
extension (RVV 1.0) to Kites. Each document is written in LaTeX and committed
here together with its built PDF.

| Document | Source | PDF | Status |
|----------|--------|-----|--------|
| Scope document | [scope_document.tex](scope_document.tex) | [scope_document.pdf](scope_document.pdf) | v1.3 |
| Design document | [design_document.tex](design_document.tex) | [design_document.pdf](design_document.pdf) | v1.0 |
| Validation report | [validation_report.tex](validation_report.tex) | [validation_report.pdf](validation_report.pdf) | v1.0 |

User-facing documentation for the vector extension is in
[`docs/VECTOR.md`](../VECTOR.md).

## Building the PDFs

The documents use [Tectonic](https://tectonic-typesetting.github.io/), a
self-contained LaTeX engine that downloads the packages it needs on first use.

```sh
brew install tectonic              # macOS; see the Tectonic site for other platforms
cd docs/vector_extension
make                               # builds every PDF
tectonic scope_document.tex        # or build a single document
```

All documents share the style file [`kitesdoc.sty`](kitesdoc.sty), which sets
fonts, colours, headings and table styles. The validation report includes
`perf_results.tex` (the scalar performance measurement) and the screenshots in
[`figures/`](figures), which are offscreen renders of the GUI.
