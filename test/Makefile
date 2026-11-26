en.img:
	dd if=/dev/zero of=$@ bs=1M count=256
	mformat -i $@
	mmd -i $@ book
	mmd -i $@ font
	mmd -i $@ dictionary
	mcopy -i $@ en/Book/*.epub ::Book
	mcopy -i $@ en/Font/*.ttf ::Font
	@if [ -d en/Dictionary ] && [ -n "$$(ls -A en/Dictionary 2>/dev/null)" ]; then \
		mcopy -s -i $@ en/Dictionary/* ::Dictionary; \
	fi
	mcopy -i $@ melonds.txt ::melonds.txt

	mdir -i $@
	mdir -i $@ book
	mdir -i $@ font
	mdir -i $@ dictionary

dict_test: dict_test.cpp ../source/stardict.cpp ../source/dict_lookup.cpp
	clang++ -std=c++17 -Wall -Wextra -fno-exceptions -fno-rtti -I../include $^ -lz -o $@
	./$@

clean:
	- rm en.img dict_test
