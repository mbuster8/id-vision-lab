#pragma once

#include "viewer_core/image_tags.h"
#include "visual_debug/image_description.h"

// viewer_core mirrors the transport's types without depending on it; the viewer, which has both, converts.
viewer_core::ImageTags toImageTags(const visual_debug::ImageDescription& description);
visual_debug::ImageDescription toDescription(const viewer_core::ImageTags& tags);
