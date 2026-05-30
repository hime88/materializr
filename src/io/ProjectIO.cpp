#include "ProjectIO.h"
#include "../core/Document.h"
#include "../modeling/Sketch.h"

#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <TopTools_FormatVersion.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pnt.hxx>
#include <gp_Dir.hxx>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <glm/glm.hpp>

namespace materializr {

// Project format history:
//   v1 — bodies only (name, visible, BREP).
//   v2 — adds per-body colour, a SKETCH section, and an optional HISTORY
//        section (operation list + per-step body diffs for undo/redo).
// The loader still reads v1 files; the saver always writes v2.

namespace {

// Write/read a single body block: "SB <id>" then BREP, terminated by "SB_END".
void writeBodyBlock(std::ofstream& ofs, int id, const TopoDS_Shape& shape) {
    ofs << "SB " << id << "\n";
    std::ostringstream brep;
    // Don't serialize the display triangulation — it bloats the file (megabytes
    // per step) and is regenerated on load. Geometry only.
    BRepTools::Write(shape, brep, Standard_False, Standard_False,
                     TopTools_FormatVersion_CURRENT);
    ofs << brep.str() << "\nSB_END\n";
}

// Reads a body block given its already-read "SB <id>" line. Returns false on EOF.
bool readBodyBlock(std::ifstream& ifs, const std::string& sbLine,
                   int& idOut, TopoDS_Shape& shapeOut) {
    std::istringstream iss(sbLine);
    std::string tok; iss >> tok >> idOut;
    std::ostringstream brep;
    std::string line;
    bool end = false;
    while (std::getline(ifs, line)) {
        if (line == "SB_END") { end = true; break; }
        brep << line << "\n";
    }
    if (!end) return false;
    BRep_Builder b;
    std::istringstream bs(brep.str());
    BRepTools::Read(shapeOut, bs, b);
    return !shapeOut.IsNull();
}

} // namespace

ProjectSaveResult ProjectIO::save(const std::string& filePath, const Document& doc,
                                  const ProjectHistory* history) {
    ProjectSaveResult result;

    std::ofstream ofs(filePath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) {
        result.errorMessage = "Failed to open file for writing: " + filePath;
        return result;
    }

    std::vector<int> bodyIds = doc.getAllBodyIds();

    ofs << "MATERIALIZR_PROJECT v2\n";
    ofs << "BODY_COUNT " << static_cast<int>(bodyIds.size()) << "\n";

    for (int id : bodyIds) {
        std::string name = doc.getBodyName(id);
        bool visible = doc.isBodyVisible(id);
        glm::vec3 c = doc.getBodyColor(id);

        ofs << "BODY_START " << id << " \"" << name << "\" " << (visible ? 1 : 0)
            << " " << c.r << " " << c.g << " " << c.b << "\n";

        try {
            const TopoDS_Shape& shape = doc.getBody(id);
            std::ostringstream brepStream;
            // Geometry only (no display triangulation) — see writeBodyBlock.
            BRepTools::Write(shape, brepStream, Standard_False, Standard_False,
                             TopTools_FormatVersion_CURRENT);
            ofs << brepStream.str();
        } catch (const std::exception& e) {
            result.errorMessage = "Failed to write BREP data for body " +
                                  std::to_string(id) + ": " + e.what();
            return result;
        }

        ofs << "\nBODY_END\n";
    }

    // --- Sketches ---
    std::vector<int> sketchIds = doc.getAllSketchIds();
    ofs << "SKETCH_COUNT " << static_cast<int>(sketchIds.size()) << "\n";
    for (int sid : sketchIds) {
        auto sk = doc.getSketch(sid);
        if (!sk) continue;
        std::string sname = doc.getSketchName(sid);
        bool svis = doc.isSketchVisible(sid);

        ofs << "SKETCH_START " << sid << " \"" << sname << "\" " << (svis ? 1 : 0)
            << " " << sk->getSourceBody() << "\n";

        const gp_Pln& pln = sk->getPlane();
        gp_Pnt o = pln.Location();
        gp_Dir z = pln.Axis().Direction();
        gp_Dir x = pln.XAxis().Direction();
        ofs << "PLANE " << o.X() << " " << o.Y() << " " << o.Z()
            << " " << z.X() << " " << z.Y() << " " << z.Z()
            << " " << x.X() << " " << x.Y() << " " << x.Z() << "\n";

        const auto& pts = sk->getPoints();
        ofs << "POINT_COUNT " << static_cast<int>(pts.size()) << "\n";
        for (const auto& p : pts)
            ofs << "P " << p.id << " " << p.pos.x << " " << p.pos.y << " "
                << (p.isConstruction ? 1 : 0) << "\n";

        const auto& lns = sk->getLines();
        ofs << "LINE_COUNT " << static_cast<int>(lns.size()) << "\n";
        for (const auto& l : lns)
            ofs << "L " << l.id << " " << l.startPointId << " " << l.endPointId << " "
                << (l.isConstruction ? 1 : 0) << "\n";

        const auto& cs = sk->getCircles();
        ofs << "CIRCLE_COUNT " << static_cast<int>(cs.size()) << "\n";
        for (const auto& c : cs)
            ofs << "C " << c.id << " " << c.centerPointId << " " << c.radius << " "
                << (c.isConstruction ? 1 : 0) << "\n";

        const auto& arcs = sk->getArcs();
        ofs << "ARC_COUNT " << static_cast<int>(arcs.size()) << "\n";
        for (const auto& a : arcs)
            ofs << "A " << a.id << " " << a.centerPointId << " " << a.startPointId << " "
                << a.endPointId << " " << a.radius << " " << (a.isConstruction ? 1 : 0) << "\n";

        const auto& spl = sk->getSplines();
        ofs << "SPLINE_COUNT " << static_cast<int>(spl.size()) << "\n";
        for (const auto& s : spl) {
            ofs << "S " << s.id << " " << (s.isConstruction ? 1 : 0) << " "
                << static_cast<int>(s.controlPointIds.size());
            for (int cp : s.controlPointIds) ofs << " " << cp;
            ofs << "\n";
        }

        const auto& polys = sk->getPolygons();
        ofs << "POLYGON_COUNT " << static_cast<int>(polys.size()) << "\n";
        for (const auto& g : polys) {
            ofs << "G " << g.id << " " << g.centerPointId << " " << g.radius << " "
                << g.sides << " " << (g.isConstruction ? 1 : 0)
                << " " << static_cast<int>(g.vertexPointIds.size());
            for (int v : g.vertexPointIds) ofs << " " << v;
            ofs << " " << static_cast<int>(g.lineIds.size());
            for (int l : g.lineIds) ofs << " " << l;
            ofs << "\n";
        }

        ofs << "SKETCH_END\n";
    }

    // --- Folders (optional, since 0.3.0) ---
    // Format:
    //   FOLDER_COUNT n
    //   for each:
    //     FOLDER id "name" visible(0|1) r g b expanded(0|1)
    //   BODY_FOLDER_COUNT m
    //   for each body that's in a folder:
    //     BODY_FOLDER bodyId folderId
    // Bodies without a FOLDER mapping stay at root (folderId == -1). Old files
    // simply omit these blocks and the loader leaves all bodies at root.
    std::vector<int> folderIds = doc.getAllFolderIds();
    ofs << "FOLDER_COUNT " << static_cast<int>(folderIds.size()) << "\n";
    for (int fid : folderIds) {
        std::string fname = doc.getFolderName(fid);
        glm::vec3 fcol = doc.getFolderColor(fid);
        bool fvis  = doc.isFolderVisible(fid);
        bool fexp  = doc.isFolderExpanded(fid);
        ofs << "FOLDER " << fid << " \"" << fname << "\" " << (fvis ? 1 : 0)
            << " " << fcol.r << " " << fcol.g << " " << fcol.b
            << " " << (fexp ? 1 : 0) << "\n";
    }
    std::vector<std::pair<int,int>> bodyFolders;
    for (int bid : bodyIds) {
        int fid = doc.getBodyFolder(bid);
        if (fid >= 0) bodyFolders.emplace_back(bid, fid);
    }
    ofs << "BODY_FOLDER_COUNT " << static_cast<int>(bodyFolders.size()) << "\n";
    for (auto [bid, fid] : bodyFolders) {
        ofs << "BODY_FOLDER " << bid << " " << fid << "\n";
    }

    // --- History (optional) ---
    if (history && history->present) {
        ofs << "HISTORY_INITIAL_COUNT " << static_cast<int>(history->initialState.size()) << "\n";
        for (const auto& [id, shape] : history->initialState)
            writeBodyBlock(ofs, id, shape);

        ofs << "HISTORY_COUNT " << static_cast<int>(history->steps.size()) << "\n";
        for (const auto& st : history->steps) {
            ofs << "STEP_START\n";
            ofs << "TYPE " << st.typeId << "\n";
            ofs << "NAME \"" << st.name << "\"\n";
            ofs << "DESC \"" << st.description << "\"\n";
            ofs << "ENABLED " << (st.enabled ? 1 : 0) << "\n";
            // Optional per-op parameter blob (since 0.3.x). Omitted entirely
            // when empty so older parsers see a familiar STEP block. Quoted
            // single-line; embedded quotes are deliberately not supported —
            // ops keep their key=value blobs to ASCII / decimal text.
            if (!st.params.empty()) {
                ofs << "PARAMS \"" << st.params << "\"\n";
            }
            ofs << "CHANGED_COUNT " << static_cast<int>(st.changed.size()) << "\n";
            for (const auto& [id, shape] : st.changed)
                writeBodyBlock(ofs, id, shape);
            ofs << "DELETED_COUNT " << static_cast<int>(st.deleted.size());
            for (int id : st.deleted) ofs << " " << id;
            ofs << "\n";
            ofs << "STEP_END\n";
        }
    }

    ofs << "END\n";

    if (!ofs.good()) {
        result.errorMessage = "I/O error while writing file: " + filePath;
        return result;
    }

    ofs.close();
    result.success = true;
    return result;
}

namespace {

// Read one sketch (everything between SKETCH_START and SKETCH_END) and add it to
// the document. `startLine` is the already-read SKETCH_START line.
void readSketch(std::ifstream& ifs, const std::string& startLine, Document& doc) {
    int sid = 0, visible = 1, source = -1;
    std::string name;
    {
        std::istringstream iss(startLine);
        std::string label;
        iss >> label >> sid;
        std::string rest;
        std::getline(iss, rest);
        auto fq = rest.find('"'), lq = rest.rfind('"');
        if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
            name = rest.substr(fq + 1, lq - fq - 1);
            std::istringstream after(rest.substr(lq + 1));
            after >> visible >> source;
        }
    }

    auto sk = std::make_shared<Sketch>();
    int maxId = 0;
    auto bump = [&](int id) { maxId = std::max(maxId, id); };

    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string tok;
        iss >> tok;
        if (tok == "SKETCH_END" || tok.empty()) break;

        if (tok == "PLANE") {
            double ox, oy, oz, zx, zy, zz, xx, xy, xz;
            iss >> ox >> oy >> oz >> zx >> zy >> zz >> xx >> xy >> xz;
            try {
                gp_Ax3 ax(gp_Pnt(ox, oy, oz), gp_Dir(zx, zy, zz), gp_Dir(xx, xy, xz));
                sk->setPlane(gp_Pln(ax));
            } catch (...) { /* keep default plane */ }
        } else if (tok == "POINT_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchPoint p; int c = 0;
                s >> t >> p.id >> p.pos.x >> p.pos.y >> c; p.isConstruction = (c != 0);
                bump(p.id); sk->addRawPoint(p);
            }
        } else if (tok == "LINE_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchLine l; int c = 0;
                s >> t >> l.id >> l.startPointId >> l.endPointId >> c; l.isConstruction = (c != 0);
                bump(l.id); sk->addRawLine(l);
            }
        } else if (tok == "CIRCLE_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchCircle c; int cf = 0;
                s >> t >> c.id >> c.centerPointId >> c.radius >> cf; c.isConstruction = (cf != 0);
                bump(c.id); sk->addRawCircle(c);
            }
        } else if (tok == "ARC_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchArc a; int c = 0;
                s >> t >> a.id >> a.centerPointId >> a.startPointId >> a.endPointId >> a.radius >> c;
                a.isConstruction = (c != 0);
                bump(a.id); sk->addRawArc(a);
            }
        } else if (tok == "SPLINE_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchSpline sp; int c = 0, cnt = 0;
                s >> t >> sp.id >> c >> cnt; sp.isConstruction = (c != 0);
                for (int k = 0; k < cnt; ++k) { int id = 0; s >> id; sp.controlPointIds.push_back(id); }
                bump(sp.id); sk->addRawSpline(sp);
            }
        } else if (tok == "POLYGON_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n && std::getline(ifs, line); ++i) {
                std::istringstream s(line); std::string t; SketchPolygon g; int c = 0, nv = 0, nl = 0;
                s >> t >> g.id >> g.centerPointId >> g.radius >> g.sides >> c >> nv;
                g.isConstruction = (c != 0);
                for (int k = 0; k < nv; ++k) { int id = 0; s >> id; g.vertexPointIds.push_back(id); }
                s >> nl;
                for (int k = 0; k < nl; ++k) { int id = 0; s >> id; g.lineIds.push_back(id); }
                bump(g.id); sk->addRawPolygon(g);
            }
        }
        // Unknown tokens inside a sketch are ignored for forward compatibility.
    }

    sk->setNextId(maxId + 1);
    sk->setSourceBody(source);
    int newId = doc.addSketch(sk, name);
    doc.setSketchVisible(newId, visible != 0);
}

} // namespace

ProjectLoadResult ProjectIO::load(const std::string& filePath, Document& doc,
                                  ProjectHistory* historyOut) {
    ProjectLoadResult result;

    std::ifstream ifs(filePath, std::ios::in);
    if (!ifs.is_open()) {
        result.errorMessage = "Failed to open file for reading: " + filePath;
        return result;
    }

    std::string headerLine;
    if (!std::getline(ifs, headerLine) ||
        headerLine.rfind("MATERIALIZR_PROJECT", 0) != 0) {
        result.errorMessage = "Invalid project file header.";
        return result;
    }

    std::string countLine;
    if (!std::getline(ifs, countLine)) {
        result.errorMessage = "Unexpected end of file reading body count.";
        return result;
    }

    int bodyCount = 0;
    {
        std::istringstream iss(countLine);
        std::string label;
        iss >> label >> bodyCount;
        if (label != "BODY_COUNT" || iss.fail()) {
            result.errorMessage = "Invalid body count line: " + countLine;
            return result;
        }
    }

    doc.clear();

    BRep_Builder builder;
    int loadedCount = 0;

    for (int i = 0; i < bodyCount; ++i) {
        std::string startLine;
        if (!std::getline(ifs, startLine)) {
            result.errorMessage = "Unexpected end of file reading body " + std::to_string(i + 1);
            return result;
        }

        // Parse: BODY_START id "name" visible [r g b]
        int bodyId = 0, visible = 1;
        std::string bodyName;
        bool hasColor = false;
        float r = 0.8f, g = 0.8f, b = 0.82f;
        {
            std::istringstream iss(startLine);
            std::string label;
            iss >> label;
            if (label != "BODY_START") {
                result.errorMessage = "Expected BODY_START, got: " + label;
                return result;
            }
            iss >> bodyId;
            std::string rest;
            std::getline(iss, rest);
            auto fq = rest.find('"'), lq = rest.rfind('"');
            if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
                bodyName = rest.substr(fq + 1, lq - fq - 1);
                std::istringstream after(rest.substr(lq + 1));
                after >> visible;
                if (after >> r >> g >> b) hasColor = true; // v2: colour present
            } else {
                bodyName = "Body " + std::to_string(bodyId);
            }
        }

        std::ostringstream brepData;
        std::string line;
        bool foundEnd = false;
        while (std::getline(ifs, line)) {
            if (line == "BODY_END") { foundEnd = true; break; }
            brepData << line << "\n";
        }
        if (!foundEnd) {
            result.errorMessage = "Missing BODY_END marker for body " + std::to_string(bodyId);
            return result;
        }

        TopoDS_Shape shape;
        std::istringstream brepStream(brepData.str());
        BRepTools::Read(shape, brepStream, builder);
        if (shape.IsNull()) {
            result.errorMessage = "Failed to read BREP data for body " + std::to_string(bodyId);
            return result;
        }

        // Preserve the saved id so the HISTORY diffs (which reference body ids)
        // stay valid after a reload.
        doc.putBody(bodyId, shape, bodyName);
        doc.setBodyVisible(bodyId, visible != 0);
        if (hasColor) doc.setBodyColor(bodyId, glm::vec3(r, g, b));
        ++loadedCount;
    }

    // After the bodies, v2 files carry a SKETCH section. Scan remaining lines and
    // dispatch; stop at END. (v1 files just have END here — nothing to do.)
    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream iss(line);
        std::string tok;
        iss >> tok;
        if (tok == "END" || tok.empty()) break;
        if (tok == "SKETCH_COUNT") {
            int n = 0; iss >> n;
            for (int i = 0; i < n; ++i) {
                std::string sline;
                if (!std::getline(ifs, sline)) break;
                std::istringstream s(sline); std::string t; s >> t;
                if (t != "SKETCH_START") break;
                readSketch(ifs, sline, doc);
            }
        } else if (tok == "FOLDER_COUNT") {
            // FOLDER blocks (0.3.0+, optional). Each line:
            //   FOLDER id "name" visible(0|1) r g b expanded(0|1)
            // We re-issue addFolder so the Document's next-id counter ticks,
            // then setFolderName/Color/Visible/Expanded etc. Note that addFolder
            // returns a NEW id rather than respecting the saved one; for now
            // that's fine because BODY_FOLDER lines below remap by saved id
            // via a local map. (Folder ids don't need to be globally stable
            // the way body ids do — no operations reference them.)
            int n = 0; iss >> n;
            std::map<int,int> savedToNewFolder;
            for (int i = 0; i < n; ++i) {
                std::string fline;
                if (!std::getline(ifs, fline)) break;
                std::istringstream fs(fline);
                std::string ftok; fs >> ftok;
                if (ftok != "FOLDER") continue;
                int savedId = 0; fs >> savedId;
                std::string rest; std::getline(fs, rest);
                auto fq = rest.find('"'), lq = rest.rfind('"');
                std::string fname = "Folder";
                int fvis = 1, fexp = 1;
                float fr = 0.8f, fg = 0.8f, fb = 0.82f;
                if (fq != std::string::npos && lq != std::string::npos && fq != lq) {
                    fname = rest.substr(fq + 1, lq - fq - 1);
                    std::istringstream after(rest.substr(lq + 1));
                    after >> fvis >> fr >> fg >> fb >> fexp;
                }
                int newId = doc.addFolder(fname);
                doc.setFolderVisible(newId, fvis != 0);
                doc.setFolderColor(newId, glm::vec3(fr, fg, fb));
                doc.setFolderExpanded(newId, fexp != 0);
                savedToNewFolder[savedId] = newId;
            }
            // BODY_FOLDER_COUNT may be on the next line OR somewhere later in
            // the same stream — peek and only consume if it's adjacent. (Save
            // writes them adjacent.)
            std::streampos restore = ifs.tellg();
            std::string maybe;
            if (std::getline(ifs, maybe)) {
                std::istringstream ms(maybe);
                std::string mt; ms >> mt;
                if (mt == "BODY_FOLDER_COUNT") {
                    int m = 0; ms >> m;
                    for (int i = 0; i < m; ++i) {
                        std::string bfline;
                        if (!std::getline(ifs, bfline)) break;
                        std::istringstream bs(bfline);
                        std::string bt; bs >> bt;
                        if (bt != "BODY_FOLDER") continue;
                        int bid = -1, savedFid = -1;
                        bs >> bid >> savedFid;
                        auto it = savedToNewFolder.find(savedFid);
                        if (it != savedToNewFolder.end()) {
                            doc.setBodyFolder(bid, it->second);
                        }
                    }
                } else {
                    ifs.seekg(restore);
                }
            }
        } else if (tok == "HISTORY_INITIAL_COUNT" && historyOut) {
            int k = 0; iss >> k;
            historyOut->present = true;
            for (int i = 0; i < k; ++i) {
                std::string sb;
                if (!std::getline(ifs, sb)) break;
                int id = 0; TopoDS_Shape sh;
                if (readBodyBlock(ifs, sb, id, sh))
                    historyOut->initialState.push_back({id, sh});
            }
        } else if (tok == "HISTORY_COUNT" && historyOut) {
            int n = 0; iss >> n;
            historyOut->present = true;
            for (int i = 0; i < n; ++i) {
                std::string sl;
                // Read up to STEP_START (tolerate stray blank lines).
                do { if (!std::getline(ifs, sl)) { sl.clear(); break; } } while (sl.empty());
                if (sl.rfind("STEP_START", 0) != 0) break;

                ProjectHistoryStep st;
                std::string l;
                while (std::getline(ifs, l)) {
                    if (l == "STEP_END") break;
                    std::istringstream ls(l);
                    std::string t; ls >> t;
                    if (t == "TYPE") { ls >> st.typeId; }
                    else if (t == "NAME" || t == "DESC" || t == "PARAMS") {
                        auto fq = l.find('"'), lq = l.rfind('"');
                        std::string v = (fq != std::string::npos && lq != fq)
                                            ? l.substr(fq + 1, lq - fq - 1) : "";
                        if      (t == "NAME")   st.name = v;
                        else if (t == "DESC")   st.description = v;
                        else                    st.params = v; // PARAMS
                    } else if (t == "ENABLED") { int e = 1; ls >> e; st.enabled = (e != 0); }
                    else if (t == "CHANGED_COUNT") {
                        int m = 0; ls >> m;
                        for (int j = 0; j < m; ++j) {
                            std::string sb;
                            if (!std::getline(ifs, sb)) break;
                            int id = 0; TopoDS_Shape sh;
                            if (readBodyBlock(ifs, sb, id, sh)) st.changed.push_back({id, sh});
                        }
                    } else if (t == "DELETED_COUNT") {
                        int p = 0; ls >> p;
                        for (int j = 0; j < p; ++j) { int id = 0; ls >> id; st.deleted.push_back(id); }
                    }
                }
                historyOut->steps.push_back(std::move(st));
            }
        }
        // Unknown sections are ignored for forward compatibility.
    }

    ifs.close();
    result.success = true;
    result.bodiesLoaded = loadedCount;
    return result;
}

} // namespace materializr
