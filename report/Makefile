.PHONY: all watch clean view

all:
	latexmk -pdf -silent -interaction=nonstopmode report.tex

watch:
	latexmk -pdf -pvc -silent -interaction=nonstopmode report.tex

clean:
	latexmk -C

view:
	latexmk -pdf -pv report.tex
