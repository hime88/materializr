#include "BrepIO.h"
#include "../core/Document.h"

#include <BRepBuilderAPI_Transform.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Standard_ErrorHandler.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Trsf.hxx>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

namespace {

// Disk convention matches StepIO: files are Z-up (the CAD-world norm — FreeCAD
// included), the scene is Y-up. Rotate about +X by ±90°.
TopoDS_Shape rotated(const TopoDS_Shape& s, double angle) {
    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)), angle);
    try {
        BRepBuilderAPI_Transform xf(s, t, /*copy=*/true);
        if (xf.IsDone() && !xf.Shape().IsNull()) return xf.Shape();
    } catch (...) {}
    return s;
}

// Add a shape's solids as bodies; fall back to shells, then faces, then the
// shape itself — the IgesIO cascade, so a faces-only file still lands visibly.
int addShapeAsBodies(const TopoDS_Shape& shape, Document& doc, int& counter) {
    int added = 0;
    for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next()) {
        doc.addBody(ex.Current(), "Imported_" + std::to_string(++counter));
        ++added;
    }
    if (added == 0) {
        for (TopExp_Explorer ex(shape, TopAbs_SHELL, TopAbs_SOLID); ex.More(); ex.Next()) {
            doc.addBody(ex.Current(), "Imported_" + std::to_string(++counter));
            ++added;
        }
    }
    if (added == 0) {
        for (TopExp_Explorer ex(shape, TopAbs_FACE, TopAbs_SHELL); ex.More(); ex.Next()) {
            doc.addBody(ex.Current(), "Imported_" + std::to_string(++counter));
            ++added;
        }
    }
    if (added == 0 && !shape.IsNull()) {
        doc.addBody(shape, "Imported_" + std::to_string(++counter));
        ++added;
    }
    return added;
}

} // namespace

ImportResult BrepIO::import(const std::string& filePath, Document& doc) {
    ImportResult result;
    try {
        OCC_CATCH_SIGNALS // kernel fault on a crafted file → the catch below
        TopoDS_Shape shape;
        BRep_Builder builder;
        if (!BRepTools::Read(shape, filePath.c_str(), builder)) {
            result.errorMessage = "Failed to read BREP file: " + filePath;
            return result;
        }
        if (shape.IsNull()) {
            result.errorMessage = "BREP file contained no shape.";
            return result;
        }
        shape = rotated(shape, M_PI * 0.5); // disk Z-up → scene Y-up

        // A top-level compound is our own multi-body export (or FreeCAD's) —
        // each child becomes its own body so they stay individually editable.
        int counter = 0;
        int imported = 0;
        if (shape.ShapeType() == TopAbs_COMPOUND) {
            for (TopoDS_Iterator it(shape); it.More(); it.Next())
                imported += addShapeAsBodies(it.Value(), doc, counter);
        } else {
            imported = addShapeAsBodies(shape, doc, counter);
        }
        if (imported == 0) {
            result.errorMessage = "No usable geometry in BREP file.";
            return result;
        }
        result.success = true;
        result.bodiesImported = imported;
        return result;
    } catch (const Standard_Failure& e) {
        result.errorMessage = std::string("OCCT error reading BREP: ") +
                              (e.GetMessageString() ? e.GetMessageString() : "unknown");
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = std::string("Error reading BREP: ") + e.what();
        return result;
    } catch (...) {
        result.errorMessage = "Unknown error reading BREP file.";
        return result;
    }
}

ExportResult BrepIO::exportFile(const std::string& filePath, const Document& doc) {
    ExportResult result;
    try {
        OCC_CATCH_SIGNALS
        std::vector<int> ids = doc.getAllBodyIds();
        if (ids.empty()) {
            result.errorMessage = "No bodies to export.";
            return result;
        }
        // Single body exports bare; several go in one compound (round-trips
        // through our own import as separate bodies again).
        TopoDS_Shape out;
        if (ids.size() == 1) {
            out = doc.getBody(ids.front());
        } else {
            TopoDS_Compound comp;
            BRep_Builder bb;
            bb.MakeCompound(comp);
            for (int id : ids) {
                const TopoDS_Shape& s = doc.getBody(id);
                if (!s.IsNull()) bb.Add(comp, s);
            }
            out = comp;
        }
        if (out.IsNull()) {
            result.errorMessage = "No exportable geometry.";
            return result;
        }
        out = rotated(out, -M_PI * 0.5); // scene Y-up → disk Z-up
        if (!BRepTools::Write(out, filePath.c_str())) {
            result.errorMessage = "Failed to write BREP file: " + filePath;
            return result;
        }
        result.success = true;
        return result;
    } catch (const Standard_Failure& e) {
        result.errorMessage = std::string("OCCT error writing BREP: ") +
                              (e.GetMessageString() ? e.GetMessageString() : "unknown");
        return result;
    } catch (const std::exception& e) {
        result.errorMessage = std::string("Error writing BREP: ") + e.what();
        return result;
    } catch (...) {
        result.errorMessage = "Unknown error writing BREP file.";
        return result;
    }
}

} // namespace materializr
