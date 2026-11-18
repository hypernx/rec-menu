#pragma once
// 表示する言語はビルド時に決める。-DLANG_JA を付けると日本語、付けないと英語。
// TR("English", "日本語") と書くと、どちらか一方の文字列になる。
#ifdef LANG_JA
#define TR(en, ja) ja
#else
#define TR(en, ja) en
#endif
