#include "GenerationLedger.h"

#include <BRepBuilderAPI_MakeShape.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>

namespace materializr {
namespace topo {

void GenerationLedger::capture(BRepBuilderAPI_MakeShape& mk,
                               const TopoDS_Shape& in, TopAbs_ShapeEnum t) {
    input = in;
    inType = t;
    generated.Clear();
    modified.Clear();
    if (in.IsNull()) return;

    TopTools_IndexedMapOfShape subs;
    TopExp::MapShapes(in, t, subs);
    for (int i = 1; i <= subs.Extent(); ++i) {
        const TopoDS_Shape& s = subs(i);
        try {
            const TopTools_ListOfShape& g = mk.Generated(s);
            if (!g.IsEmpty()) generated.Add(s, g);
        } catch (...) {}
        try {
            const TopTools_ListOfShape& m = mk.Modified(s);
            if (!m.IsEmpty()) modified.Add(s, m);
        } catch (...) {}
    }
}

} // namespace topo
} // namespace materializr
