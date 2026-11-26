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
/*!
A Page stores the flowed output from the XML parse and is equipped
to render a full left and right screen of text.

A Book contains a vector of Pages.
*/
#pragma once

#include <nds.h>
#include <string>
#include "book.h"
#include "text.h"

struct WordBox {
	u16 bufStart;
	u16 bufLen;
	u8 screenIdx; // 0 = screenleft, 1 = screenright
	u8 x;
	u8 y;
	u8 w;
	u8 h;
	bool isCjk;
};

class Page {
	class Book *book;
	void DrawNumber(Text *ts);

 public:
	//! UTF-8 chars, allocated per-page at parse time, to exact length.
	u8 *buf;
	//! Length of buf.
	int length;
	//! In a book-long char buffer, where would i begin?
	int start;
	//! Ditto, for end char. 
	int end;
	Page(Book *b);
	Page(Book *b, Text *t);
	~Page();
	u8*  GetBuffer() { return buf; }
	int  GetLength() { return length; }
	//! Copy src to buf for len bytes.
	u8   SetBuffer(u8 *src, u16 len); 
	void Cache(FILE *fp);
	void Draw(Text *ts);
	//! Extract raw UTF-8 token (or up to 8-char CJK window) for dictionary lookup.
	std::string ExtractWord(const WordBox& box) const;
};
