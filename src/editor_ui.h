#pragma once

// -----------------------------------------------------------------------------
//  Toolbar control ids and editor tool modes
// -----------------------------------------------------------------------------
enum ControlId : int {
    ID_SHAPE_BASE  = 200,   // 200..207: one button per addable shape
    ID_TOOL_SELECT = 220,
    ID_TOOL_MOVE   = 221,
    ID_TOOL_ROTATE = 222,
    ID_DELETE      = 230,
    ID_DELETE_ALL  = 231,
};

enum class EditMode { Select, Move, Rotate };
