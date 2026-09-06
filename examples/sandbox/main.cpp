/*
 * @file main.cpp
 * @brief Scratch application for experimenting with the Catalyst libraries.
 * @details This example intentionally does nothing. It exists as a ready-made target to drop
 * throwaway code into while trying something out; the focused examples alongside it show how the
 * individual modules are meant to be used.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/catalyst.hpp>

int main()
{
  catalyst::catalyst_version_anchor();

  {
    using namespace catalyst::resource;

    // A braced list becomes an object only when *every* element is a {string, value} pair.
    // Give the number list a key and the whole thing is an object.
    json::value json_object = {{"Hello", 0}, {"World", 1}, {"Numbers", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}}};

    printf("%s -> %s\n", json_object.is_object() ? "object" : "array", json::dump(json_object).c_str());

    // A mixed list is always an array. To make an element an object, wrap the pair once more so
    // that element is itself a one-pair braced list: {{"Hello", 0}} is {"Hello":0}.
    json::value json_array = {{{"Hello", 0}}, {{"World", 1}}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}};

    printf("%s -> %s\n", json_array.is_array() ? "array" : "object", json::dump(json_array).c_str());

  } // end of JSON resource scope

  return 0;
}
