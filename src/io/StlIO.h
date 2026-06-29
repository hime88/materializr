#pragma once
#include <string>

#include "StepIO.h" // reuse ImportResult

class Document;

namespace materializr {

// STL import. Reads an ASCII or binary STL mesh (triangle soup), sews the
// facets into a shell/solid, merges coplanar facets, and adds the result as a
// body. The imported body is tessellated geometry — thousands of planar faces,
// not analytic surfaces — so it is useful as a reference body, a boolean tool,
// or for re-export, but is not parametrically editable like a sketched feature.
//
// PARKED: the menu entry is currently disabled (see StlImportPlugin). A faceted
// body of this size makes the viewport's per-frame per-face hover-pick and the
// per-triangle wireframe crawl, and decimating it enough to be smooth lost too
// much fidelity. Kept in the tree for a future mesh-native rework (display the
// triangulation directly instead of converting to a heavy B-rep). Export still
// lives in StlExport (see StlExportPlugin); this is import only.
class StlIO {
public:
    static ImportResult import(const std::string& filePath, Document& doc);
};

} // namespace materializr
