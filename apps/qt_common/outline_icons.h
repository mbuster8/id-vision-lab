#pragma once

#include <QColor>
#include <QIcon>

namespace qt_common {

// The applications' icons: original outline drawings in the style of Heroicons, stroked in code so the build
// needs no Qt SVG module and no third-party icon licence.
enum class OutlineIcon {
    Pencil,
    Duplicate,
    Trash,
    Pause,
    Play,
    Stop,
    ChevronUp,
    ChevronDown,
    Copy,
    Clear,
    Search,
    Options,
    Info,
    Warning,
    Error,
    Folder,
    Send,
    Camera,
    EyeOff,
    StepBack,
    StepForward,
    Back10,
    Forward10,
    Restart,
    Loop,
    Fast,
    Record,
    Overlays,
    Tick,
    Cross
};

// Go and halt, as macOS's own green and red, readable on a light window and a dark one.
inline const QColor kGo{0x34, 0xC7, 0x59};
inline const QColor kHalt{0xFF, 0x3B, 0x30};

// Strokes the shape at any size, in the application palette's colour at paint time, so the icon follows a
// switch between dark and light: WindowText normally, Highlight when on (a checked toggle), HighlightedText
// when selected, PlaceholderText when disabled.
QIcon makeOutlineIcon(OutlineIcon shape);

// The shape in a colour of its own, whatever the palette: Play, Stop and Record filled, the others stroked;
// PlaceholderText when disabled, as every disabled icon.
QIcon makeColouredIcon(OutlineIcon shape, QColor colour);

} // namespace qt_common
