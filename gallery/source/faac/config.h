/* FAAC (https://github.com/knik0/faac) をこのアプリに組み込むための設定。
   本来は meson が作るファイル。 */
#pragma once
#define PACKAGE "faac"
#define PACKAGE_VERSION "2.0.0"
#define FAAC_SBR_DECIMATION 1
#define MAX_CHANNELS 2
/* HAVE_SSE2 / WORDS_BIGENDIAN / FAAC_STATS は使わない */
