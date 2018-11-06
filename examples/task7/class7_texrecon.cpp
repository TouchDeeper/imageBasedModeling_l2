/*
 * Copyright (C) 2015, Nils Moehrle
 * TU Darmstadt - Graphics, Capture and Massively Parallel Computing
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD 3-Clause license. See the LICENSE.txt file for details.
 */

#include <iostream>
#include <fstream>
#include <vector>

#include <util/timer.h>
#include <util/system.h>
#include <util/file_system.h>
#include <core/mesh_io_ply.h>

#include "texturing/util.h"
#include "texturing/timer.h"
#include "texturing/debug.h"
#include "texturing/texturing.h"
#include "texturing/progress_counter.h"

#include "arguments.h"

int main(int argc, char **argv) {
#ifdef RESEARCH
    std::cout << "******************************************************************************" << std::endl
              << " Due to use of the -DRESEARCH=ON compile option, this program is licensed "     << std::endl
              << " for research purposes only. Please pay special attention to the gco license."  << std::endl
              << "******************************************************************************" << std::endl;
#endif

    util::system::register_segfault_handler();
    Timer timer;
    timer.measure("Start");
    util::WallTimer wtimer;
    Arguments conf;
    try {
        conf = parse_args(argc, argv);
    } catch (std::invalid_argument & ia) {
        std::cerr << ia.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }

    if (!util::fs::dir_exists(util::fs::dirname(conf.out_prefix).c_str())) {
        std::cerr << "Destination directory does not exist!" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    //==================================Load Mesh ===================================//
    std::cout << "Load and prepare mesh: " << std::endl;
    core::TriangleMesh::Ptr mesh;
    try {
        mesh = core::geom::load_ply_mesh(conf.in_mesh);
    } catch (std::exception& e) {
        std::cerr << "\tCould not load mesh: "<< e.what() << std::endl;
        std::exit(EXIT_FAILURE);
    }


    //=================================Prepare Mesh=================================//
    core::VertexInfoList::Ptr vertex_infos = core::VertexInfoList::create(mesh);
    tex::prepare_mesh(vertex_infos, mesh);



    //=================================Geneatring texture views=====================//
    std::size_t const num_faces = mesh->get_faces().size() / 3;
    std::cout << "Generating texture views: " << std::endl;
    tex::TextureViews texture_views;
    tex::generate_texture_views(conf.in_scene, &texture_views);
    write_string_to_file(conf.out_prefix + ".conf", conf.to_string());
    timer.measure("Loading");


    //===============================Building adjacency graph=======================//
    std::cout << "Building adjacency graph: " << std::endl;
    tex::Graph graph(num_faces);
    tex::build_adjacency_graph(mesh, vertex_infos, &graph);
    wtimer.reset();


/****** 2015/12/11 yangliang  *************/



    //===============================View Selection ================================//
    // if labeling file does not exist, compute a view label for each facet via MRF
    if (conf.labeling_file.empty()) {
        std::cout << "View selection:" << std::endl;
        std::size_t const num_faces = mesh->get_faces().size() / 3;
        tex::DataCosts data_costs(num_faces, texture_views.size());

        // if data cost file does not exist, compute the data cost
        if (conf.data_cost_file.empty()) {

            /******************* compute data cost  ***********************************/
            tex::calculate_data_costs(mesh, &texture_views, conf.settings, &data_costs);
            /**************************************************************************/

            if (conf.write_intermediate_results) {
                std::cout << "\tWriting data cost file... " << std::flush;
                    ST::save_to_file(data_costs, conf.out_prefix + "_data_costs.spt");
                std::cout << "done." << std::endl;
            }
        } else {  // if the data cost file exists, just load it from the file
            std::cout << "\tLoading data cost file... " << std::flush;
            try {
                ST::load_from_file(conf.data_cost_file, &data_costs);
            } catch (util::FileException e) {
                std::cout << "failed!" << std::endl;
                std::cerr << e.what() << std::endl;
                std::exit(EXIT_FAILURE);
            }
            std::cout << "done." << std::endl;
        }
        timer.measure("Calculating data costs");


        // MRF Optimization for view selection
        tex::view_selection(data_costs, &graph, conf.settings);
        timer.measure("Running MRF optimization");

        /* Write labeling to file. */
        if (conf.write_intermediate_results) {
            std::vector<std::size_t> labeling(graph.num_nodes());
            for (std::size_t i = 0; i < graph.num_nodes(); ++i) {
                labeling[i] = graph.get_label(i);
            }
            vector_to_file(conf.out_prefix + "_labeling.vec", labeling);
        }
    } else {  // if labeling file has existed, just read it from the file
        std::cout << "Loading labeling from file... " << std::flush;

        /* Load labeling from file. */
        std::vector<std::size_t> labeling = vector_from_file<std::size_t>(conf.labeling_file);
        if (labeling.size() != graph.num_nodes()) {
            std::cerr << "Wrong labeling file for this mesh/scene combination... aborting!" << std::endl;
            std::exit(EXIT_FAILURE);
        }

        /* Transfer labeling to graph. */
        for (std::size_t i = 0; i < labeling.size(); ++i) {
            const std::size_t label = labeling[i];
            if (label > texture_views.size()){
                std::cerr << "Wrong labeling file for this mesh/scene combination... aborting!" << std::endl;
                std::exit(EXIT_FAILURE);
            }
            graph.set_label(i, label);
        }

        std::cout << "done." << std::endl;
    }
    std::cout << "\tTook: " << wtimer.get_elapsed_sec() << "s" << std::endl;



    //=============================================texture_atlases====================================================//
    tex::TextureAtlases texture_atlases;
    {
        /* Create texture patches and adjust them. */
        tex::TexturePatches texture_patches;
        tex::VertexProjectionInfos vertex_projection_infos;
        std::cout << "Generating texture patches:" << std::endl;
        tex::generate_texture_patches(graph,
                                      mesh,
                                      vertex_infos,
                                      &texture_views,
                                      &vertex_projection_infos,
                                      &texture_patches);

        // Global seam leveling
        if (conf.settings.global_seam_leveling) {
            std::cout << "Running global seam leveling:" << std::endl;
            tex::global_seam_leveling(graph,
                                      mesh,
                                      vertex_infos,
                                      vertex_projection_infos,
                                      &texture_patches);
            timer.measure("Running global seam leveling");
        } else {
            ProgressCounter texture_patch_counter("Calculating validity masks for texture patches", texture_patches.size());
            #pragma omp parallel for schedule(dynamic)
            for (std::size_t i = 0; i < texture_patches.size(); ++i) {
                texture_patch_counter.progress<SIMPLE>();
                TexturePatch::Ptr texture_patch = texture_patches[i];
                std::vector<math::Vec3f> patch_adjust_values(texture_patch->get_faces().size() * 3, math::Vec3f(0.0f));
                texture_patch->adjust_colors(patch_adjust_values);
                texture_patch_counter.inc();
            }
            timer.measure("Calculating texture patch validity masks");
        }

        //======================================local seam leveling===========================//
        // Local seam leveling (Poisson Editing???)
        if (conf.settings.local_seam_leveling) {
            std::cout << "Running local seam leveling:" << std::endl;
            tex::local_seam_leveling(graph, mesh, vertex_projection_infos, &texture_patches);
        }
        timer.measure("Running local seam leveling");


        //====================================Generating textgure atlases====================//
        // Generate texture atlases
        /* Generate texture atlases. */
        std::cout << "Generating texture atlases:" << std::endl;
        tex::generate_texture_atlases(&texture_patches, &texture_atlases);
    }


       //=================================Write Obj model=====================================//
    /* Create and write out obj model. */
    {
        std::cout << "Building objmodel:" << std::endl;
        tex::Model model;
        tex::build_model(mesh, texture_atlases, &model);
        timer.measure("Building OBJ model");

        std::cout << "\tSaving model... " << std::flush;
        tex::Model::save(model, conf.out_prefix);
        std::cout << "done." << std::endl;
        timer.measure("Saving");
    }

    std::cout << "Whole texturing procedure took: " << wtimer.get_elapsed_sec() << "s" << std::endl;
    timer.measure("Total");
    if (conf.write_timings) {
        timer.write_to_file(conf.out_prefix + "_timings.csv");
    }

    if (conf.write_view_selection_model) {
        texture_atlases.clear();
        std::cout << "Generating debug texture patches:" << std::endl;
        {
            tex::TexturePatches texture_patches;
            generate_debug_embeddings(&texture_views);
            tex::VertexProjectionInfos vertex_projection_infos; // Will only be written
            tex::generate_texture_patches(graph, mesh, vertex_infos, &texture_views, &vertex_projection_infos, &texture_patches);
            tex::generate_texture_atlases(&texture_patches, &texture_atlases);
        }

        std::cout << "Building debug objmodel:" << std::endl;
        {
            tex::Model model;
            tex::build_model(mesh, texture_atlases, &model);
            std::cout << "\tSaving model... " << std::flush;
            tex::Model::save(model, conf.out_prefix + "_view_selection");
            std::cout << "done." << std::endl;
        }
    }
}
