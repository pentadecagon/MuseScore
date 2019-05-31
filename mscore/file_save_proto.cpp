//=============================================================================
//  MuseScore
//  Music Composition & Notation
//
//  Copyright (C) 2002-2014 Werner Schweer
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2
//  as published by the Free Software Foundation and appearing in
//  the file LICENCE.GPL
//=============================================================================

/**
 File handling: loading and saving.
 */

#include "all.h"
#include "config.h"
#include "globals.h"
#include "musescore.h"
#include "scoreview.h"
#include "exportmidi.h"
#include "libmscore/xml.h"
#include "libmscore/element.h"
#include "libmscore/ledgerline.h"
#include "libmscore/note.h"
#include "libmscore/rest.h"
#include "libmscore/sig.h"
#include "libmscore/clef.h"
#include "libmscore/key.h"
#include "instrdialog.h"
#include "libmscore/score.h"
#include "libmscore/page.h"
#include "libmscore/dynamic.h"
#include "file.h"
#include "libmscore/style.h"
#include "libmscore/tempo.h"
#include "libmscore/select.h"
#include "preferences.h"
#include "playpanel.h"
#include "libmscore/staff.h"
#include "libmscore/part.h"
#include "libmscore/utils.h"
#include "libmscore/barline.h"
#include "palette.h"
#include "symboldialog.h"
#include "libmscore/slur.h"
#include "libmscore/hairpin.h"
#include "libmscore/ottava.h"
#include "libmscore/textline.h"
#include "libmscore/pedal.h"
#include "libmscore/trill.h"
#include "libmscore/volta.h"
#include "newwizard.h"
#include "libmscore/timesig.h"
#include "libmscore/box.h"
#include "libmscore/excerpt.h"
#include "libmscore/system.h"
#include "libmscore/tuplet.h"
#include "libmscore/keysig.h"
#include "magbox.h"
#include "libmscore/measure.h"
#include "libmscore/undo.h"
#include "libmscore/repeatlist.h"
#include "scoretab.h"
#include "libmscore/beam.h"
#include "libmscore/stafftype.h"
#include "seq.h"
#include "libmscore/revisions.h"
#include "libmscore/lyrics.h"
#include "libmscore/segment.h"
#include "libmscore/tempotext.h"
#include "libmscore/sym.h"
#include "libmscore/image.h"
#include "libmscore/stafflines.h"
#include "synthesizer/msynthesizer.h"
#include "svggenerator.h"
#include "scorePreview.h"
#include "scorecmp/scorecmp.h"
#include "extension.h"
#include "tourhandler.h"
#include <fstream>

#include "libmscore/chordlist.h"
#include "libmscore/mscore.h"
#include "thirdparty/qzip/qzipreader_p.h"
#include <glog/logging.h>
#include <map>

namespace Ms {

static const SysStaff* FindSysStaff(const Element* p) {
//    const MeasureBase* m = p->findMeasureBase();
    const System* sys = nullptr;
        for (auto* x = p; x; x = x->parent()) {
            if (x->isSystem()) {
                sys = toSystem(x);
                break;
            }
        }
        const int idx = p->staffIdx();
    if (sys == nullptr || idx < 0 || idx >= sys->staves()->size()) return 0;
    return sys->staff(idx);
}

static std::tuple<double, int, double> SortKey(const MusicOCR::Piece& p) {
      return {p.x(), p.line(), p.y()};
      }

static bool IsSlur(const MusicOCR::Piece& piece) {
      using MusicOCR::Ref1;
      for (auto x : {Ref1::SlurEndBelow, Ref1::SlurEndAbove, Ref1::SlurStartBelow, Ref1::SlurStartAbove}) {
            if (piece.ref1() == x) {
                  return true;
                  }
            }
      return false;
      }

static bool MaybeDuplicate(const MusicOCR::Piece& piece) {
      using MusicOCR::Ref1;
      auto ref1 = piece.ref1();
      return ref1 >= Ref1::RestWhole && ref1 <= Ref1::Rest_64
      || ref1 <= Ref1::StemDown_64;
      }

// returns true if this staff should not used for training.
static void MarkUnsupported(MusicOCR::Staff& staff) {
      using MusicOCR::Ref1;
      using MusicOCR::Ref2;
      using MusicOCR::Piece;
      using MusicOCR::Staff;
      const int minline = Staff::MinLine;
      const int maxline = Staff::MaxLine;
      const double miny = staff.y() - staff.dy() * Staff::TopLineIndex * 0.5;
      const double maxy = miny + staff.dy() * (Staff::OcrHeight - 1) * 0.5;
      std::map<Ref1::ERef1, double> xref1;
      std::map<std::pair<Ref2::ERef2, int>, double> xref2;
      for (int k=0; k< staff.piece_size(); ++k) {
            Piece& piece = *staff.mutable_piece(k);
            if (piece.name() != "" || piece.piece_error() != "") {
                  continue;
                  }
            if (piece.line() < minline || piece.line() > maxline) {
                  piece.set_piece_error("Line outside range");
                  continue;
                  }
            if (piece.y() < miny || piece.y() > maxy) {
                  piece.set_piece_error("y out of range");
                  continue;
                  }
            if (IsSlur(piece)) {
                  continue;
                  }

           if (auto ref1 = piece.ref1()) {
                  double& x = xref1[ref1];
                  if (x > 0) {
                      CHECK_LE(x, piece.x());
                      if (MaybeDuplicate(piece) && piece.x() - x < 0.1 * staff.dy()) {
                            piece.set_piece_error("Something is wrong");
                            staff.mutable_piece()->erase(staff.mutable_piece()->begin() + k);
                            }
                      else if (piece.x() - x < 0.9 * staff.dy()) { // Parameter
                          piece.set_piece_error("x distance too low");
                      }
                  }
                  x = piece.x();
              }
              if (auto ref2 = piece.ref2()) {
                  double& x = xref2[{ref2, piece.line()}];
                  if (x > 0) {
                      CHECK_LE(x, piece.x());
                      if (piece.x() - x < 0.9 * staff.dy()) { // Parameter
                            piece.set_piece_error("x distance too low");
                      }
                  }
                  x = piece.x();
              }
          }
      }



static void SortLayout(MusicOCR::Layout* layout) {
      auto& stafflist = *layout->mutable_staff();
      std::sort(stafflist.begin(), stafflist.end(), [](const MusicOCR::Staff& a, const MusicOCR::Staff& b) {return a.y() < b.y();});
      for (auto& staff : *layout->mutable_staff()) {
            auto& pieces = *staff.mutable_piece();
            std::sort(pieces.begin(), pieces.end(), [](const MusicOCR::Piece& a, const MusicOCR::Piece& b){return SortKey(a) < SortKey(b);});
            MarkUnsupported(staff);
            }
      }

void savePiecesProto(const QList<Element*> & vel, const QString& fname, double mag ) {
      std::map<const SysStaff*, MusicOCR::Staff*> m;
      MusicOCR::Layout layout;
      for (const Element* el : vel) {
            if (!el->visible()) continue;
            if (const StaffLines* stafflines = dynamic_cast<const StaffLines*>(el)) {
                  const SysStaff* ss = FindSysStaff(stafflines);
                  CHECK(ss);
                  MusicOCR::Staff*& mstaff = m[ss];
                  if (!mstaff) mstaff = layout.add_staff();
                  stafflines->updateStaff(mstaff);
                  }
            }
      for (MusicOCR::Staff& ms : *layout.mutable_staff()) {
            ms.set_x1(ms.x1() * mag);
            ms.set_x2(ms.x2() * mag);
            ms.set_dy(ms.dy() * mag);
            ms.set_y(ms.y() * mag);
            }
      static const string ignore[] = {"Text", "Image", "Page", "VBox", "LayoutBreak"};
      std::set<const Element*> xset;
      for (const Element* p : vel) {
            if (!xset.insert(p).second) continue;
            if (!p->visible()) continue;
            if (!dynamic_cast<const StaffLines*>(p)) {
                  auto* ss = FindSysStaff(p);
                  if (!ss) {
                        if (std::find(begin(ignore), end(ignore), p->name()) == end(ignore)) {
                              cerr << "Bad Element: " << p->name() << endl;
                              }
                        continue;
                        }
                  MusicOCR::Staff* mstaff = m[ss];
                  if (!mstaff) {
                        cerr << "incosistent staff " << p->name() << endl;
                        continue;
                        }
                  if (const LedgerLine* ll = dynamic_cast<const LedgerLine*>(p)) {
                        } else {
                            int n = mstaff->piece_size();
                              p->AddToProto(mstaff, mag);
                              for (int k = n; k < mstaff->piece_size(); ++k) {
                                  mstaff->mutable_piece(k)->set_tick(p->tick());
                              }
                        }
                  }
            }
      SortLayout(&layout);
      std::ofstream ost((fname+".pb").toStdString().c_str(),ios::binary);
      ost << layout.SerializeAsString();
      cerr << "Found Staff: " << layout.staff_size() << endl;
      }

} // namespace Ms
