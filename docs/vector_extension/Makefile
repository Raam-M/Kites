# Builds the vector-extension PDFs with Tectonic (https://tectonic-typesetting.github.io/).
# Add new documents (without the .tex extension) to DOCS.
TEX  := tectonic
DOCS := scope_document design_document validation_report

all: $(addsuffix .pdf,$(DOCS))

%.pdf: %.tex kitesdoc.sty
	$(TEX) $<

.PHONY: all
