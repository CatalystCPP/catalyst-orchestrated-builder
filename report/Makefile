all: report.pdf

report.pdf: report.tex catalyst.tikz
	pdflatex -interaction=nonstopmode $<
	sleep 1
	pdflatex -interaction=nonstopmode $<
	rm -f **/*.aux *.aux report.log report.out

view:
	open report.pdf > /dev/null 2>&1 &

clean:
	rm -f *.aux report.log report.out report.toc report.pdf

.PHONY: all view cleean report.pdf # make report.pdf phony so I can rerun freely
