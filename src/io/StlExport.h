#pragma once
#include <string>
#include <TopoDS_Shape.hxx>

class Document;

namespace materializr {

struct StlExportOptions {
    // Export quality. An STL is for slicing/manufacturing, so favour smoothness
    // over triangle count (this is a one-shot export, not the live viewport).
    // The old 0.5 rad angular gave only ~3 facets across a 90° fillet — visibly
    // rippled when sliced; 0.1 rad (~5.7°) gives ~16 and reads as smooth.
    double linearDeflection = 0.02;  // max chord error, mm (smaller = more triangles)
    double angularDeflection = 0.1;  // max facet angle on curves, radians (~5.7°)
    bool binary = true;              // Binary STL (smaller, faster) vs ASCII
};

struct StlExportResult {
    bool success = false;
    std::string errorMessage;
    int triangleCount = 0;
};

class StlExport {
public:
    // Export all visible bodies to a single STL file
    static StlExportResult exportFile(const std::string& filePath, const Document& doc,
                                       const StlExportOptions& options = {});

    // Export a single shape
    static StlExportResult exportShape(const std::string& filePath, const TopoDS_Shape& shape,
                                        const StlExportOptions& options = {});
};

} // namespace materializr
