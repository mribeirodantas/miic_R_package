#include "orientation.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#include <algorithm>  // std::minmax, std::remove, std::transform
#define _USE_MATH_DEFINES
#include <cmath>
#include <map>

#include "get_information.h"
#include "linear_allocator.h"
#include "proba_orientation.h"

namespace miic {
namespace reconstruction {

using std::string;
using std::vector;
using std::fabs;
using namespace miic::computation;
using namespace miic::structure;
using namespace miic::utility;

namespace {

constexpr double kEpsI3 = 1.0e-10;

bool acceptProba(double proba, double ori_proba_ratio) {
  return (1 - proba) / proba < ori_proba_ratio;
}

// y2x: probability that there is an arrow from node y to x
// x2y: probability that there is an arrow from node x to y
void updateAdj(Environment& env, int x, int y, double y2x, double x2y) {
  env.edges(x, y).proba_head = x2y;
  if (acceptProba(x2y, env.ori_proba_ratio))
    env.edges(x, y).status = 2;
  env.edges(y, x).proba_head = y2x;
  if (acceptProba(y2x, env.ori_proba_ratio))
    env.edges(y, x).status = 2;
}

}  // anonymous namespace

vector<vector<string>> orientationProbability(Environment& environment) {
  // Get all unshielded triples X -- Z -- Y
  vector<Triple> triples;
  const auto& edge_list = environment.connected_list;
  for (auto iter0 = begin(edge_list); iter0 != end(edge_list); ++iter0) {
    int posX = iter0->X, posY = iter0->Y;

    for (auto iter1 = iter0 + 1; iter1 != end(edge_list); ++iter1) {
      int posX1 = iter1->X, posY1 = iter1->Y;
      if (posY1 == posX && !environment.edges(posY, posX1).status)
        triples.emplace_back(Triple{posY, posX, posX1});
      else if (posY1 == posY && !environment.edges(posX, posX1).status)
        triples.emplace_back(Triple{posX, posY, posX1});
      if (posX1 == posX && !environment.edges(posY, posY1).status)
        triples.emplace_back(Triple{posY, posX, posY1});
      else if (posX1 == posY && !environment.edges(posX, posY1).status)
        triples.emplace_back(Triple{posX, posY, posY1});
    }
  }
  if (triples.empty())
    return vector<vector<string>>();

  // Compute the 3-point mutual info (N * I'(X;Y;Z|{ui})) for each triple
  vector<double> I3_list(triples.size());
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic)
#endif
  for (size_t i = 0; i < triples.size(); ++i) {
    int X{triples[i][0]}, Z{triples[i][1]}, Y{triples[i][2]};
    const auto& ui_list = environment.edges(X, Y).shared_info->ui_list;
    vector<int> ui_no_z(ui_list);
    ui_no_z.erase(remove(begin(ui_no_z), end(ui_no_z), Z), end(ui_no_z));

    auto block = getInfo3Point(environment, X, Y, Z, ui_no_z);
    I3_list[i] = block.Ixyz_ui - block.kxyz_ui;
  }

  // Compute the arrowhead probability of each edge endpoint
  vector<ProbaArray> probas_list =
      getOriProbasList(triples, I3_list, environment.is_contextual,
          environment.latent_orientation, environment.degenerate,
          environment.propagation, environment.half_v_structure);
  // update probas_list for possible inconsistencies
  class ProbaArrayMap : public std::map<std::pair<int, int>, double> {
   public:
    auto insert_or_update(std::pair<int, int> key, double proba) {
      auto iter_key = this->find(key);
      if (iter_key == this->end()) {
        return this->insert({std::move(key), proba});
      } else {
        // Update if proba is more probable
        if (fabs(iter_key->second - 0.5) < fabs(proba - 0.5))
          this->at(key) = proba;
        return std::make_pair(iter_key, false);
      }
    }
  } proba_map;

  for (size_t i = 0; i < triples.size(); i++) {
    const auto& triple = triples[i];
    const auto& probas = probas_list[i];
    proba_map.insert_or_update({triple[1], triple[0]}, probas[0]);  // 1 -> 0
    proba_map.insert_or_update({triple[0], triple[1]}, probas[1]);  // 0 -> 1
    proba_map.insert_or_update({triple[2], triple[1]}, probas[2]);  // 2 -> 1
    proba_map.insert_or_update({triple[1], triple[2]}, probas[3]);  // 1 -> 2
  }
  // Update probabilities
  std::transform(begin(triples), end(triples), begin(probas_list),
      [&proba_map](const auto& triple) {
        return ProbaArray{proba_map.at({triple[1], triple[0]}),
            proba_map.at({triple[0], triple[1]}),
            proba_map.at({triple[2], triple[1]}),
            proba_map.at({triple[1], triple[2]})};
      });
  // Update adj matrix
  for (size_t i = 0; i < triples.size(); i++) {
    const auto& triple = triples[i];
    const auto& probas = probas_list[i];
    updateAdj(environment, triple[0], triple[1], probas[0], probas[1]);
    updateAdj(environment, triple[1], triple[2], probas[2], probas[3]);
  }
  // Write output
  vector<vector<string>> orientations{{"source1", "p1", "p2", "target", "p3",
      "p4", "source2", "NI3", "Conflict"}};
  for (size_t i = 0; i < triples.size(); i++) {
    const auto& triple = triples[i];
    const auto& probas = probas_list[i];

    double I3 = fabs(I3_list[i]) < kEpsI3 ? 0 : I3_list[i];
    int info_sign = (I3 > 0) - (I3 < 0);
    // -1: v-structure, 1: non-v-structure
    int proba_sign = (probas[1] > 0.5 && probas[2] > 0.5) ? -1 : 1;
    // conflict if I3 is non-zero but info_sign is different from proba_sign,
    // since positive I3 indicates non-v-structure and negative I3 indicates
    // v-structure.
    string conflict = (I3 != 0 && info_sign != proba_sign) ? "1" : "0";

    using std::to_string;
    orientations.emplace_back(std::initializer_list<string>{
        environment.nodes[triple[0]].name,
        to_string(probas[0]), to_string(probas[1]),
        environment.nodes[triple[1]].name,
        to_string(probas[2]), to_string(probas[3]),
        environment.nodes[triple[2]].name,
        to_string(I3_list[i]), conflict});
  }
  return orientations;
}

}  // namespace reconstruction
}  // namespace miic
