#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/tree_schema.h"
#include "../../src/tree_data.h"

int main(int argc, char *argv[])
{
    unsigned long x, suma = 0;

    fprintf(stdout, "%8lu struct lllys_module\n", x = sizeof(struct lllys_module)); suma += x;
    fprintf(stdout, "%8lu struct lllys_submodule\n", x = sizeof(struct lllys_submodule)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_binary\n", x = sizeof(struct lllys_type_info_binary)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_bit\n", x = sizeof(struct lllys_type_bit)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_bits\n", x = sizeof(struct lllys_type_info_bits)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_dec64\n", x = sizeof(struct lllys_type_info_dec64)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_enum\n", x = sizeof(struct lllys_type_enum)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_enums\n", x = sizeof(struct lllys_type_info_enums)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_ident\n", x = sizeof(struct lllys_type_info_ident)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_inst\n", x = sizeof(struct lllys_type_info_inst)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_num\n", x = sizeof(struct lllys_type_info_num)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_lref\n", x = sizeof(struct lllys_type_info_lref)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_str\n", x = sizeof(struct lllys_type_info_str)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type_info_union\n", x = sizeof(struct lllys_type_info_union)); suma += x;
    fprintf(stdout, "%8lu struct lllys_type\n", x = sizeof(struct lllys_type)); suma += x;
    fprintf(stdout, "%8lu struct lllys_iffeature\n", x = sizeof(struct lllys_iffeature)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node\n", x = sizeof(struct lllys_node)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_container\n", x = sizeof(struct lllys_node_container)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_choice\n", x = sizeof(struct lllys_node_choice)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_leaf\n", x = sizeof(struct lllys_node_leaf)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_leaflist\n", x = sizeof(struct lllys_node_leaflist)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_list\n", x = sizeof(struct lllys_node_list)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_anyxml\n", x = sizeof(struct lllys_node_anydata)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_uses\n", x = sizeof(struct lllys_node_uses)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_grp\n", x = sizeof(struct lllys_node_grp)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_case\n", x = sizeof(struct lllys_node_case)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_inout\n", x = sizeof(struct lllys_node_inout)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_notif\n", x = sizeof(struct lllys_node_notif)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_rpc_action\n", x = sizeof(struct lllys_node_rpc_action)); suma += x;
    fprintf(stdout, "%8lu struct lllys_node_augment\n", x = sizeof(struct lllys_node_augment)); suma += x;
    fprintf(stdout, "%8lu struct lllys_refine_mod_list\n", x = sizeof(struct lllys_refine_mod_list)); suma += x;
    fprintf(stdout, "%8lu struct lllys_refine\n", x = sizeof(struct lllys_refine)); suma += x;
    fprintf(stdout, "%8lu struct lllys_deviate\n", x = sizeof(struct lllys_deviate)); suma += x;
    fprintf(stdout, "%8lu struct lllys_deviation\n", x = sizeof(struct lllys_deviation)); suma += x;
    fprintf(stdout, "%8lu struct lllys_import\n", x = sizeof(struct lllys_import)); suma += x;
    fprintf(stdout, "%8lu struct lllys_include\n", x = sizeof(struct lllys_include)); suma += x;
    fprintf(stdout, "%8lu struct lllys_revision\n", x = sizeof(struct lllys_revision)); suma += x;
    fprintf(stdout, "%8lu struct lllys_tpdf\n", x = sizeof(struct lllys_tpdf)); suma += x;
    fprintf(stdout, "%8lu struct lllys_unique\n", x = sizeof(struct lllys_unique)); suma += x;
    fprintf(stdout, "%8lu struct lllys_feature\n", x = sizeof(struct lllys_feature)); suma += x;
    fprintf(stdout, "%8lu struct lllys_restr\n", x = sizeof(struct lllys_restr)); suma += x;
    fprintf(stdout, "%8lu struct lllys_when\n", x = sizeof(struct lllys_when)); suma += x;
    fprintf(stdout, "%8lu struct lllys_ident\n", x = sizeof(struct lllys_ident)); suma += x;
    fprintf(stdout, "SCHEMA TREE SUM %8lu\n\n", suma);

    suma = 0;
    fprintf(stdout, "%8lu struct lllyd_attr\n", x = sizeof(struct lllyd_attr)); suma += x;
    fprintf(stdout, "%8lu struct lllyd_node\n", x = sizeof(struct lllyd_node)); suma += x;
    fprintf(stdout, "%8lu struct lllyd_node_leaf_list\n", x = sizeof(struct lllyd_node_leaf_list)); suma += x;
    fprintf(stdout, "%8lu struct lllyd_node_anyxml\n", x = sizeof(struct lllyd_node_anydata)); suma += x;
    fprintf(stdout, "%8lu struct lllyd_difflist\n", x = sizeof(struct lllyd_difflist)); suma += x;
    fprintf(stdout, "DATA TREE SUM %8lu\n\n", suma);

	return 0;
}

