# Third-party code

| Folder | What | Version | License | Used by |
|---|---|---|---|---|
| `rcheevos/` | RetroAchievements' client library | 12.5.0 | MIT | the game (RetroAchievements) |
| `lzma/` | the LZMA SDK's 7z decoder (`C/` of 7-Zip, the files its `Util/7z` sample builds from, without `7zFile.c`'s use: Setup reads through SDL) | 7-Zip 26.05, from github.com/ip7z/7zip | public domain | Setup (`tools/setup/sevenzip.c`): `.7z` input |
| `stb_vorbis/` | Ogg Vorbis decoder, `stb_vorbis.c` | 1.22, nothings/stb commit 1ee679ca2ef753a528db5ba6801e1067b40481b8 | public domain or MIT | Setup (`tools/setup/vc.c`): Virtual Console audio |
