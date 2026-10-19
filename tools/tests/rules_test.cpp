// Checks src/rules.h. Runs anywhere:  g++ -std=c++17 -Isrc tools/tests/rules_test.cpp -o /tmp/rules_test && /tmp/rules_test
#include <cstdio>
#include <cstdlib>

#include "rules.h"

static int failures = 0;
#define CHECK(c)                                                  \
    do {                                                          \
        if (!(c)) {                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);      \
            failures++;                                           \
        }                                                         \
    } while (0)

static std::wstring Stem(const wchar_t* n) {
    std::wstring s, e;
    SplitName(n, s, e);
    return s;
}
static std::wstring Ext(const wchar_t* n) {
    std::wstring s, e;
    SplitName(n, s, e);
    return e;
}

int main() {
    CHECK(Stem(L"IMG_1234.jpg") == L"IMG_1234" && Ext(L"IMG_1234.jpg") == L".jpg");
    CHECK(Stem(L"a.b.c.png") == L"a.b.c" && Ext(L"a.b.c.png") == L".png");
    CHECK(Stem(L"noext") == L"noext" && Ext(L"noext").empty());
    CHECK(Stem(L".hidden") == L".hidden" && Ext(L".hidden").empty());
    CHECK(Stem(L"trailing.") == L"trailing" && Ext(L"trailing.") == L".");

    CHECK(TrimSpaces(L"  a b  ") == L"a b");
    CHECK(TrimSpaces(L"   ").empty());
    CHECK(TrimSpaces(L"x") == L"x");

    for (wchar_t c : std::wstring(L"\\/:*?\"<>|")) CHECK(!NameCharOk(c));
    CHECK(!NameCharOk(L'\n') && !NameCharOk(L'\t') && !NameCharOk(0x7f));
    CHECK(NameCharOk(L'a') && NameCharOk(L' ') && NameCharOk(L'é') && NameCharOk(L'.') && NameCharOk(0x4e2d));
    CHECK(NameCharOk(0xD83D) && NameCharOk(0xDE00));  // the halves of a surrogate pair

    CHECK(ReservedName(L"con") && ReservedName(L"NUL") && ReservedName(L"Aux") && ReservedName(L"prn"));
    CHECK(ReservedName(L"COM1") && ReservedName(L"lpt9") && ReservedName(L"con.backup") && ReservedName(L"CON "));
    CHECK(!ReservedName(L"COM0") && !ReservedName(L"COM10") && !ReservedName(L"console") && !ReservedName(L"holiday"));

    CHECK(NameProblem(L"holiday 2024") == nullptr);
    CHECK(NameProblem(L"") != nullptr);
    CHECK(NameProblem(L"a/b") != nullptr && NameProblem(L"what?") != nullptr);
    CHECK(NameProblem(L"NUL") != nullptr);

    CHECK(Unpremultiply(0, 0) == 0);
    CHECK(Unpremultiply(255, 255) == 255 && Unpremultiply(10, 255) == 10);
    CHECK(Unpremultiply(64, 128) == 127 || Unpremultiply(64, 128) == 128);
    CHECK(Unpremultiply(200, 100) == 255);  // can't exceed white when the data is slightly off

    // ordering
    const SortOrder nameAsc{SORT_NAME, false}, nameDesc{SORT_NAME, true}, dateNew{SORT_DATE, true}, dateOld{SORT_DATE, false};
    CHECK(OrderBefore(nameAsc, -1, 5, 1) && !OrderBefore(nameAsc, 1, 1, 5) && !OrderBefore(nameAsc, 0, 1, 1));
    CHECK(!OrderBefore(nameDesc, -1, 0, 0) && OrderBefore(nameDesc, 1, 0, 0) && !OrderBefore(nameDesc, 0, 0, 0));
    CHECK(OrderBefore(dateNew, 1, 9, 3) && !OrderBefore(dateNew, -1, 3, 9));  // newer first, names ignored
    CHECK(OrderBefore(dateOld, 1, 3, 9) && !OrderBefore(dateOld, -1, 9, 3));
    CHECK(OrderBefore(dateNew, -1, 4, 4) && !OrderBefore(dateNew, 1, 4, 4));  // same date: name A to Z
    CHECK(OrderBefore(dateOld, -1, 4, 4) && !OrderBefore(dateOld, 1, 4, 4));
    CHECK(!OrderBefore(dateNew, 0, 4, 4) && !OrderBefore(nameAsc, 0, 4, 4));  // never "before" itself

    SortOrder o = nameAsc;
    o = PressSortKey(o, SORT_NAME);
    CHECK(o == nameDesc);
    o = PressSortKey(o, SORT_NAME);
    CHECK(o == nameAsc);
    o = PressSortKey(o, SORT_DATE);
    CHECK(o == dateNew);
    o = PressSortKey(o, SORT_DATE);
    CHECK(o == dateOld);
    o = PressSortKey(o, SORT_NAME);  // switching resets the direction
    CHECK(o == nameAsc);
    for (SortOrder v : {nameAsc, nameDesc, dateNew, dateOld}) CHECK(UnpackSort(PackSort(v)) == v);
    CHECK(UnpackSort(0xFFFFFFF0u) == nameAsc);  // junk in the registry means the default

    if (failures) return 1;
    std::printf("all rules checks passed\n");
    return 0;
}
