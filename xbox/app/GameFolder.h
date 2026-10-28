// Where the FINAL FANTASY XI install is (with the PlayOnlineViewer folder beside it, as retail puts
// them). The first of:
//   - LocalState\game.txt's first line: a folder named there (a network share's, say)
//   - SquareEnix\FINAL FANTASY XI beside the app: the loose app with the game uploaded next to it
//     (tools/xbox.py setup, docs/xbox-install.md)
//   - LocalState\SquareEnix\FINAL FANTASY XI: a copy made by the first XI on Xbox
// "" when none of them holds FFXiMain.dll.
#pragma once

#include <string>

std::wstring FindGameFolder();
