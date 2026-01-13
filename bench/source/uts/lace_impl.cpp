#include <lace.h>
#include <alloca.h>

#include "../util.hpp"
#include "config.hpp"
#include "external/uts.h"

TASK_2(result, uts_lace_run, int, depth, Node *, parent)

result uts_lace_run_CALL(lace_worker* worker, int depth, Node *parent) {
    int numChildren, childType;
    int parentHeight = parent->height;

    result r(depth, 1, 0);

    numChildren = uts_numChildren(parent);
    childType   = uts_childType(parent);

    parent->numChildren = numChildren;

    if (numChildren > 0) {
        int i, j;

        for (i = 0; i < numChildren; i++) {
            Node *child = (Node*)alloca(sizeof(Node));

            child->type = childType;
            child->height = parentHeight + 1;
            child->numChildren = -1;

            for (j = 0; j < computeGranularity; j++) {
                rng_spawn(parent->state.state, child->state.state, i);
            }

            uts_lace_run_SPAWN(worker, depth + 1, child);
        }

        for (i = 0; i < numChildren; i++) {
            result c = uts_lace_run_SYNC(worker);

            r.maxdepth = max(r.maxdepth, c.maxdepth);
            r.size += c.size;
            r.leaves += c.leaves;
        }
    } else {
        r.leaves = 1;
    }

    return r;
}

extern "C" {

void lace_uts_init_bridge(int workers) {
    lace_start(workers, 0, 0);
}

void lace_uts_stop_bridge() {
    lace_stop();
}

void lace_uts_bridge(int tree, result* out_res) {
    setup_tree(tree);

    Node root;
    uts_initRoot(&root, type);

    *out_res = uts_lace_run(0, &root);
}

}