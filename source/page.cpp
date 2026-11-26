/*
 Copyright (C) 2007-2009 Ray Haleblian
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software Foundation,
 Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 
 To contact the copyright holder: rayh23@sourceforge.net
 */

#include "page.h"
#include "app.h"
#include "dict_lookup.h"

#include <string.h>
#include <list>

Page::Page(Book *b)
{
	book = b;
	buf = NULL;
	length = 0;
	start = 0;
}

Page::~Page()
{
	if(buf) delete buf;
}

u8 Page::SetBuffer(u8 *src, u16 len)
{
	//! Write to offscreen buffer. NYI
	if(buf) delete buf;
	buf = new u8[len];
	strncpy((char*)buf, (char*)src, len);
	length = len;
	return 0;
}

void Page::Cache(FILE *fp)
{
	if(!buf) return;
	fwrite((const char*)buf, 1, strlen((char*)buf), fp);
}

std::string Page::ExtractWord(const WordBox& box) const
{
	if (!buf || box.bufStart >= length) return std::string();

	if (!box.isCjk) {
		int avail = length - box.bufStart;
		int take = (box.bufLen < avail) ? box.bufLen : avail;
		return std::string(reinterpret_cast<const char*>(buf + box.bufStart), take);
	}

	// CJK: gather up to 8 characters starting from bufStart for longest-prefix lookup.
	std::string out;
	int pos = box.bufStart;
	int chars = 0;
	while (pos < length && chars < 8) {
		u8 b = buf[pos];
		if (b == '\n' || b == ' ' || b == '\t') break;
		if (b == TEXT_BOLD_ON || b == TEXT_BOLD_OFF ||
		    b == TEXT_ITALIC_ON || b == TEXT_ITALIC_OFF) {
			pos++;
			continue;
		}
		uint32_t cp = 0;
		size_t step = DictText::Utf8Decode(reinterpret_cast<const char*>(buf + pos),
		                                   static_cast<size_t>(length - pos), &cp);
		if (step == 0) break;
		if (DictText::IsPunctCodepoint(cp)) break;
		out.append(reinterpret_cast<const char*>(buf + pos), step);
		pos += static_cast<int>(step);
		chars++;
	}
	return out;
}

void Page::Draw(Text *ts)
{
	iprintf("hi\n");
	//! Write to offscreen buffer, then blit to video memory, for both screens.
	ts->InitPen();
	ts->linebegan = false;
	ts->italic = false;
	ts->bold = false;

	App* app = book ? book->GetApp() : nullptr;
	std::vector<WordBox>* words = app ? &app->pageWords : nullptr;
	if (words) {
		words->clear();
	}

	// Double-buffer each screen in ts->offscreen and blit during VBlank to avoid flicker.
	u16* pushscreen = ts->GetScreen();
	ts->SetScreen(ts->offscreen);
	ts->ClearScreen();

	// Track active word bounding box while rendering.
	bool inWord = false;
	WordBox curWord = {};
	u8 lineH = ts->GetHeight();
	if (lineH < 6) lineH = 12;

	auto flushWord = [&]() {
		if (!inWord) return;
		inWord = false;
		if (!words || curWord.bufLen == 0 || curWord.w == 0) return;
		// Skip standalone punctuation tokens.
		std::string raw(reinterpret_cast<const char*>(buf + curWord.bufStart), curWord.bufLen);
		if (!DictText::CleanToken(raw).empty()) {
			words->push_back(curWord);
		}
	};

	auto computeBoxY = [&](u8 baselineY, u8& outY, u8& outH) {
		int asc = (lineH * 4) / 5 + 1;
		int top = static_cast<int>(baselineY) - asc;
		if (top < 0) top = 0;
		int h = static_cast<int>(lineH) + 2;
		if (top + h > ts->display.height) h = ts->display.height - top;
		outY = static_cast<u8>(top);
		outH = static_cast<u8>(h > 0 ? h : 1);
	};

	u8 curScreenIdx = 0;
	u16 i = 0;
	while (i < length)
	{
		u32 c = buf[i];
		if (c == '\n')
		{
			flushWord();
			// line break, page breaking if necessary
			i++;
			
			if (ts->GetPenY() + ts->GetHeight() * ts->linespacing
				> ts->display.height - ts->margin.bottom)
			{
				// Move to right page
				if (curScreenIdx == 0) {
					ts->CopyScreen(ts->offscreen, ts->screenleft);
					curScreenIdx = 1;
					ts->SetScreen(ts->offscreen);
					ts->ClearScreen();
					ts->InitPen();
					ts->linebegan = false;
				}
				else
					break;
			}
			else if (ts->linebegan) {
				ts->PrintNewLine();
			}
		} else if (c == TEXT_BOLD_ON) {
			i++;
			ts->bold = true;
			if (inWord) curWord.bufLen = i - curWord.bufStart;
		} else if (c == TEXT_BOLD_OFF) {
			i++;
			ts->bold = false;
			if (inWord) curWord.bufLen = i - curWord.bufStart;
		} else if (c == TEXT_ITALIC_ON) {
			i++;
			ts->italic = true;
			if (inWord) curWord.bufLen = i - curWord.bufStart;
		} else if (c == TEXT_ITALIC_OFF) {
			i++;
			ts->italic = false;
			if (inWord) curWord.bufLen = i - curWord.bufStart;
		} else {
			u16 charStart = i;
			if (c > 127) {
				u8 step = ts->GetCharCode((char*)&(buf[i]), &c);
				i += (step > 0) ? step : 1;
			} else {
				i++;
			}

			u8 penXBefore = ts->GetPenX();
			u8 penYBefore = ts->GetPenY();

			if (ts->bold && ts->italic)
				ts->PrintChar(c, TEXT_STYLE_BOLDITALIC);
			else if (ts->italic)
				ts->PrintChar(c, TEXT_STYLE_ITALIC);
			else if (ts->bold)
				ts->PrintChar(c, TEXT_STYLE_BOLD);
			else
				ts->PrintChar(c, TEXT_STYLE_REGULAR);

			u8 penXAfter = ts->GetPenX();
			ts->linebegan = true;

			if (words) {
				if (c == ' ' || c == '\t') {
					flushWord();
				} else if (DictText::IsCjkCodepoint(c)) {
					flushWord();
					if (!DictText::IsPunctCodepoint(c)) {
						WordBox cjk = {};
						cjk.bufStart = charStart;
						cjk.bufLen = i - charStart;
						cjk.screenIdx = curScreenIdx;
						cjk.x = penXBefore;
						computeBoxY(penYBefore, cjk.y, cjk.h);
						int w = static_cast<int>(penXAfter) - static_cast<int>(penXBefore);
						cjk.w = static_cast<u8>(w > 0 ? w : lineH);
						cjk.isCjk = true;
						words->push_back(cjk);
					}
				} else {
					if (!inWord) {
						inWord = true;
						curWord = {};
						curWord.bufStart = charStart;
						curWord.bufLen = i - charStart;
						curWord.screenIdx = curScreenIdx;
						curWord.x = penXBefore;
						computeBoxY(penYBefore, curWord.y, curWord.h);
						int w = static_cast<int>(penXAfter) - static_cast<int>(penXBefore);
						curWord.w = static_cast<u8>(w > 0 ? w : 1);
						curWord.isCjk = false;
					} else {
						curWord.bufLen = i - curWord.bufStart;
						int w = static_cast<int>(penXAfter) - static_cast<int>(curWord.x);
						if (w > 0) curWord.w = static_cast<u8>(w < 255 ? w : 255);
					}
				}
			}
		}
	}

	flushWord();

	if (curScreenIdx == 0) {
		// Short page fit entirely on left screen; blit left and prepare blank right screen.
		ts->CopyScreen(ts->offscreen, ts->screenleft);
		ts->SetScreen(ts->offscreen);
		ts->ClearScreen();
	}

	DrawNumber(ts);
	ts->CopyScreen(ts->offscreen, ts->screenright);
	ts->SetScreen(pushscreen);
}

void Page::DrawNumber(Text *ts)
{
	//! Draw page number on current screen.
	char msg[128];
	
	// Find out if the page is bookmarked or not
	bool isBookmark = false;
	u16 pagecurrent = book->GetPosition();
	u16 pagecount = book->GetPageCount();
	std::list<u16>* bookmarks = book->GetBookmarks();
	for (std::list<u16>::iterator i = bookmarks->begin(); 
		 i != bookmarks->end(); i++) {
		if (*i == pagecurrent)
		{
			isBookmark = true;
			break;
		}
	}
	if (isBookmark) {
		if(pagecount == 1)
			sprintf((char*)msg,"[ %d* ]",pagecurrent+1);
		else if(pagecurrent == 0) 
			sprintf((char*)msg,"[ %d* >",pagecurrent+1);
		else if(pagecurrent == pagecount-1)
			sprintf((char*)msg,"< %d* ]",pagecurrent+1);
		else
			sprintf((char*)msg,"< %d* >",pagecurrent+1);
	} else {
		if(pagecount == 1)
			sprintf((char*)msg,"[ %d ]",pagecurrent+1);
		else if(pagecurrent == 0) 
			sprintf((char*)msg,"[ %d >",pagecurrent+1);
		else if(pagecurrent == pagecount-1)
			sprintf((char*)msg,"< %d ]",pagecurrent+1);
		else
			sprintf((char*)msg,"< %d >",pagecurrent+1);
	}

	// Position page number in horizontal proportion
	// to our current progress in the book.
	int stringwidth = ts->GetStringAdvance(msg);
	int region = ts->display.width - ts->margin.left - ts->margin.right - stringwidth;
	int location;
	if(pagecount == 1)
		location = (ts->display.width/2) - (stringwidth/2);
	else
		location = ts->margin.left
			+ (int)((float)region * (float)pagecurrent / (float)(pagecount-1));

	ts->SetPen((u8)location,250);
	ts->PrintString(msg);
}
