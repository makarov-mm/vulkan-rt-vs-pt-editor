#pragma once

// -----------------------------------------------------------------------------
//  Toolbar: control IDs and editor tool modes
// -----------------------------------------------------------------------------
enum {
    ID_SHAPE_BASE  = 200,   // 200..207: one button per addable shape
    ID_TOOL_SELECT = 220,
    ID_TOOL_MOVE   = 221,
    ID_TOOL_ROTATE = 222,
    ID_DELETE      = 230,
    ID_DELETE_ALL  = 231,
};

enum EditMode { MODE_SELECT = 0, MODE_MOVE, MODE_ROTATE };
