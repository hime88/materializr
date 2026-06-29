#include "StlIO.h"
#include "../core/Document.h"

#include <RWStl.hxx>
#include <Poly_Triangulation.hxx>
#include <Poly_Triangle.hxx>

#include <TopoDS.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Face.hxx>
#include <TopExp_Explorer.hxx>

#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>

#include <Bnd_Box.hxx>
#include <gp_Trsf.hxx>
#include <gp_Ax1.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>

#include <Standard_Failure.hxx>
#include <Standard_ErrorHandler.hxx>

#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace materializr {

ImportResult StlIO::import(const std::string& filePath, Document& doc) {
    ImportResult result;

    // OCCT can throw Standard_Failure (or raise a kernel signal) on malformed
    // meshes or while sewing degenerate facets. Catch it so a bad import fails
    // gracefully instead of aborting the process — on Android an uncaught fault
    // shows up as an instant crash.
    try {
    OCC_CATCH_SIGNALS

    Handle(Poly_Triangulation) mesh = RWStl::ReadFile(filePath.c_str());
    if (mesh.IsNull() || mesh->NbTriangles() == 0) {
        result.errorMessage = "Failed to read STL file (empty or unrecognized): " + filePath;
        return result;
    }

    // A sewing tolerance scaled to the model size: STL stores single-precision
    // floats, so coincident vertices on shared edges rarely match exactly. Too
    // tight and the shell never closes into a solid; too loose and distinct
    // features weld together. A small fraction of the bounding-box diagonal is a
    // robust middle ground.
    Bnd_Box box;
    for (Standard_Integer i = 1; i <= mesh->NbNodes(); ++i) {
        box.Add(mesh->Node(i));
    }
    double diag = 0.0;
    if (!box.IsVoid()) {
        double xmin, ymin, zmin, xmax, ymax, zmax;
        box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
        const double dx = xmax - xmin, dy = ymax - ymin, dz = zmax - zmin;
        diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const double sewTol = std::max(1e-6, diag * 1e-6);

    BRepBuilderAPI_Sewing sewing(sewTol);
    int facesAdded = 0;
    for (Standard_Integer i = 1; i <= mesh->NbTriangles(); ++i) {
        Standard_Integer n1, n2, n3;
        mesh->Triangle(i).Get(n1, n2, n3);
        const gp_Pnt p1 = mesh->Node(n1);
        const gp_Pnt p2 = mesh->Node(n2);
        const gp_Pnt p3 = mesh->Node(n3);

        // Skip degenerate (zero-area) facets — MakeFace would fail on them and
        // they contribute nothing to the surface.
        BRepBuilderAPI_MakePolygon poly(p1, p2, p3, /*Close=*/Standard_True);
        if (!poly.IsDone()) continue;
        BRepBuilderAPI_MakeFace mf(poly.Wire(), /*OnlyPlane=*/Standard_True);
        if (!mf.IsDone()) continue;
        sewing.Add(mf.Face());
        ++facesAdded;
    }

    if (facesAdded == 0) {
        result.errorMessage = "STL contained no usable facets.";
        return result;
    }

    sewing.Perform();
    TopoDS_Shape sewn = sewing.SewedShape();
    if (sewn.IsNull()) {
        result.errorMessage = "Failed to sew STL facets into a surface.";
        return result;
    }

    // Promote closed shells to solids so booleans / volume queries work; fall
    // back to the sewn shell(s) if a shell isn't watertight.
    TopoDS_Shape solidified = sewn;
    {
        bool madeSolid = false;
        BRepBuilderAPI_MakeSolid mkSolid;
        for (TopExp_Explorer ex(sewn, TopAbs_SHELL); ex.More(); ex.Next()) {
            mkSolid.Add(TopoDS::Shell(ex.Current()));
            madeSolid = true;
        }
        if (madeSolid && mkSolid.IsDone() && !mkSolid.Solid().IsNull()) {
            TopoDS_Shape solidShape = mkSolid.Solid(); // copy-construct to a base handle
            // STL facet winding is not guaranteed outward; a reversed winding
            // yields an inside-out solid (negative volume) that breaks booleans
            // and volume queries. Flip it so material is on the inside.
            GProp_GProps vprops;
            BRepGProp::VolumeProperties(solidShape, vprops);
            if (vprops.Mass() < 0.0) solidShape.Reverse();
            solidified = solidShape;
        }
    }

    // Merge adjacent coplanar facets into single faces. A printed/CAD STL is
    // mostly flat regions tessellated into many triangles; this collapses each
    // flat region back to one face, taking the result from tens of thousands of
    // faces to a handful — far more selectable and far cheaper downstream.
    TopoDS_Shape finalShape = solidified;
    try {
        ShapeUpgrade_UnifySameDomain unify(solidified, /*unifyEdges=*/Standard_True,
                                           /*unifyFaces=*/Standard_True,
                                           /*concatBSplines=*/Standard_False);
        unify.Build();
        if (!unify.Shape().IsNull()) finalShape = unify.Shape();
    } catch (...) {
        // Keep the un-unified shape on any failure — it's still valid geometry.
    }

    // STL, like STEP, is conventionally Z-up; this viewer is Y-up. Rotate -90°
    // about X so the model stands on its natural ground plane. (Mirrors the
    // reorient in StepIO::import.)
    gp_Trsf zUpToYUp;
    zUpToYUp.SetRotation(gp_Ax1(gp_Pnt(0.0, 0.0, 0.0), gp_Dir(1.0, 0.0, 0.0)),
                         -M_PI * 0.5);
    try {
        BRepBuilderAPI_Transform xf(finalShape, zUpToYUp, /*copy=*/true);
        if (xf.IsDone() && !xf.Shape().IsNull()) finalShape = xf.Shape();
    } catch (...) {}

    if (finalShape.IsNull()) {
        result.errorMessage = "STL import produced an empty shape.";
        return result;
    }

    doc.addBody(finalShape, "Imported_STL");
    result.success = true;
    result.bodiesImported = 1;
    return result;
    } catch (const Standard_Failure& e) {
        result.success = false;
        result.errorMessage = std::string("STL import failed: ") + e.GetMessageString();
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = std::string("STL import failed: ") + e.what();
    } catch (...) {
        result.success = false;
        result.errorMessage = "STL import failed: unrecognized error";
    }
    return result;
}

} // namespace materializr
