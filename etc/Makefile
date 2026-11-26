filesystem/en/Book/quickstart.epub: quickstart/EPUB/text/ch001.xhtml quickstart/EPUB/text/title_page.xhtml quickstart/EPUB/nav.xhtml quickstart/EPUB/toc.ncx
	rm -f $@
	(cd quickstart && zip -X0 ../$@ mimetype && zip -Xr ../$@ META-INF EPUB -x '*.DS_Store')

clean:
	- rm filesystem/en/Book/quickstart.epub

