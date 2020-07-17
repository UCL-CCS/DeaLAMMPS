#ifndef FE_PROBLEM_H
#define FE_PROBLEM_H


#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <string>
#include <sys/stat.h>
#include <math.h>
#include <numeric>
#include <random>

#include "boost/archive/text_oarchive.hpp"
#include "boost/archive/text_iarchive.hpp"
#include "boost/property_tree/ptree.hpp"
#include "boost/property_tree/json_parser.hpp"
#include "boost/foreach.hpp"
//#include "boost/filesystem.hpp"

// Specifically built header files
#include "read_write.h"
#include "math_calc.h"
#include "scale_bridging_data.h"

// Reduction model based on spline comparison
//#include "strain2spline.h"

// To avoid conflicts...
// pointers.h in input.h defines MIN and MAX
// which are later redefined in petsc headers
#undef  MIN
#undef  MAX

#include <deal.II/grid/tria_boundary_lib.h>
#include <deal.II/fe/fe_tools.h>
#include <deal.II/fe/fe_dgq.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/multithread_info.h>
#include <deal.II/base/conditional_ostream.h>
#include <deal.II/base/utilities.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/dynamic_sparsity_pattern.h>
#include <deal.II/lac/petsc_parallel_vector.h>
#include <deal.II/lac/petsc_parallel_sparse_matrix.h>
#include <deal.II/lac/petsc_solver.h>
#include <deal.II/lac/petsc_precondition.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/lac/sparsity_tools.h>
#include <deal.II/distributed/shared_tria.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/manifold_lib.h>
#include <deal.II/grid/grid_tools.h>
#include <deal.II/grid/grid_in.h>
#include <deal.II/grid/grid_out.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>
#include <deal.II/base/symmetric_tensor.h>
#include <deal.II/grid/filtered_iterator.h>
#include <deal.II/base/mpi.h>

#include "FE_problem_type.h"
#include "drop_weight.h"
#include "dogbone.h"
#include "compact_tension.h"
#include "FE.h"

namespace CONT
{
	using namespace dealii;

	template <int dim>
			FEProblem<dim>::FEProblem (MPI_Comm dcomm, int pcolor, int fe_deg, int quad_for, 
							const int n_total_processes)
			:
					n_world_processes (n_total_processes),
					FE_communicator (dcomm),
					n_FE_processes (Utilities::MPI::n_mpi_processes(FE_communicator)),
					this_FE_process (Utilities::MPI::this_mpi_process(FE_communicator)),
					FE_pcolor (pcolor),
					dcout (std::cout,(this_FE_process == 0)),
					triangulation(FE_communicator),
					dof_handler (triangulation),
					fe (FE_Q<dim>(fe_deg), dim),
					quadrature_formula (quad_for),
					history_fe (1),
					history_dof_handler (triangulation)
		{}



	template <int dim>
			FEProblem<dim>::~FEProblem ()
			{
					dof_handler.clear ();
			}

	template <int dim>
			void FEProblem<dim>::make_grid ()
			{
					std::string problem_class;
					problem_class = input_config.get<std::string>("problem type.class");
					
					if (problem_class == "drop weight"){
							problem_type = (ProblemType<dim>*) new DropWeight<dim>(input_config);
					}
					else if (problem_class == "dogbone"){
							problem_type = (ProblemType<dim>*) new Dogbone<dim>(input_config);
					}
					else if (problem_class == "compact tension"){
							problem_type = (ProblemType<dim>*) new CompactTension<dim>(input_config);
					}

					else {
							std::cout << "Problem type not implemented" << std::endl;
							exit(1);
					}

					problem_type->make_grid(triangulation);

					// Check that the FEM is not passed less ranks than cells
					if ( triangulation.n_active_cells() < n_FE_processes &&
									triangulation.n_active_cells() < n_world_processes ){
							dcout << "Exception: Cells < ranks in FE communicator... " << std::endl;
							exit(1);
					}

					visualise_mesh(triangulation);

					// Saving triangulation, not usefull now and costly...
					//sprintf(filename, "%s/mesh.tria", macrostatelocout.c_str());
					//std::ofstream oss(filename);
					//boost::archive::text_oarchive oa(oss, boost::archive::no_header);
					//triangulation.save(oa, 0);
							
					dcout << "    Number of active cells:       "
							<< triangulation.n_active_cells()
							<< " (by partition:";
					for (unsigned int p=0; p<n_FE_processes; ++p)
							dcout << (p==0 ? ' ' : '+')
									<< (GridTools::
													count_cells_with_subdomain_association (triangulation,p));
					dcout << ")" << std::endl;
			}

	template<int dim>
	void FEProblem<dim>::visualise_mesh(parallel::shared::Triangulation<dim> &triangulation)
	{
	if (this_FE_process==0){
		char filename[1024];
		sprintf(filename, "%s/3D_mesh.eps", macrostatelocout.c_str());
		std::ofstream out (filename);
		GridOut grid_out;
		grid_out.write_eps (triangulation, out);
		dcout << "    mesh .eps written to " << filename << std::endl;	
		}
	}




	template <int dim>
			void FEProblem<dim>::setup_system ()
			{
					dof_handler.distribute_dofs (fe);
					locally_owned_dofs = dof_handler.locally_owned_dofs();
					DoFTools::extract_locally_relevant_dofs (dof_handler,locally_relevant_dofs);

					history_dof_handler.distribute_dofs (history_fe);

					n_local_cells
							= GridTools::count_cells_with_subdomain_association (triangulation,
											triangulation.locally_owned_subdomain ());
					local_dofs_per_process = dof_handler.n_locally_owned_dofs_per_processor();

					hanging_node_constraints.clear ();
					DoFTools::make_hanging_node_constraints (dof_handler,
									hanging_node_constraints);
					hanging_node_constraints.close ();

					DynamicSparsityPattern sparsity_pattern (locally_relevant_dofs);
					DoFTools::make_sparsity_pattern (dof_handler, sparsity_pattern,
									hanging_node_constraints, false);
					SparsityTools::distribute_sparsity_pattern (sparsity_pattern,
									local_dofs_per_process,
									FE_communicator,
									locally_relevant_dofs);

					mass_matrix.reinit (locally_owned_dofs,
									locally_owned_dofs,
									sparsity_pattern,
									FE_communicator);
					system_matrix.reinit (locally_owned_dofs,
									locally_owned_dofs,
									sparsity_pattern,
									FE_communicator);
					system_rhs.reinit (locally_owned_dofs, FE_communicator);

					newton_update_displacement.reinit (dof_handler.n_dofs());
					incremental_displacement.reinit (dof_handler.n_dofs());
					displacement.reinit (dof_handler.n_dofs());
					//old_displacement.reinit (dof_handler.n_dofs());
					//for (unsigned int i=0; i<dof_handler.n_dofs(); ++i) old_displacement(i) = 0.0;

					newton_update_velocity.reinit (dof_handler.n_dofs());
					incremental_velocity.reinit (dof_handler.n_dofs());
					velocity.reinit (dof_handler.n_dofs());

					dcout << "    Number of degrees of freedom: "
							<< dof_handler.n_dofs()
							<< " (by partition:";
					for (unsigned int p=0; p<n_FE_processes; ++p)
							dcout << (p==0 ? ' ' : '+')
									<< (DoFTools::
													count_dofs_with_subdomain_association (dof_handler,p));
					dcout << ")" << std::endl;
			}



	template <int dim>
			CellData<dim> FEProblem<dim>::get_microstructure ()
			{
					std::string 	distribution_type;

					distribution_type = input_config.get<std::string>("materials info.distribution.style");
					if (distribution_type == "uniform"){
							dcout << " generating uniform distribution of materials... " << std::endl;

							std::vector<double> proportions;
							BOOST_FOREACH(boost::property_tree::ptree::value_type &v,
											input_config.get_child("materials info.distribution.proportions."))
							{
									proportions.push_back(std::stod(v.second.data()));
							}

							// check length of materials list and proportions list are the same
							if (mdtype.size() != proportions.size())
							{
									dcout<< "Materials list and proportions list must be the same length" <<std::endl;
									exit(1);
							}
							// Generate nanostructure on rank 0, then broadcast it to other ranks
							if (this_FE_process == 0){
								celldata.generate_nanostructure_uniform(triangulation, proportions);
							}
							else {
								celldata.composition.resize(triangulation.n_active_cells());
							}
							MPI_Bcast(&(celldata.composition[0]), triangulation.n_active_cells(), MPI_INT, 0, FE_communicator);

					}	

					return celldata;
			}


	template <int dim>
			void FEProblem<dim>::assign_microstructure (typename DoFHandler<dim>::active_cell_iterator cell, CellData<dim> celldata,
							std::string &mat)
			{

					// Filling identity matrix
					Tensor<2,dim> idmat;
					idmat = 0.0; for (unsigned int i=0; i<dim; ++i) idmat[i][i] = 1.0;

					unsigned int n = cell->active_cell_index();
					mat = mdtype[ celldata.get_composition(n) ];	

			}




			template <int dim>
					void FEProblem<dim>::setup_quadrature_point_history ()
					{
							triangulation.clear_user_data();
							{
									std::vector<PointHistory<dim> > tmp;
									tmp.swap (quadrature_point_history);
							}
							quadrature_point_history.resize (n_local_cells *
											quadrature_formula.size());
							char filename[1024];

							// Set materials initial stiffness tensors
							std::vector<SymmetricTensor<4,dim> > stiffness_tensors (mdtype.size());
							std::vector<double > densities (mdtype.size());

							dcout << "    Importing initial stiffnesses and densities..." << std::endl;
							for(unsigned int imd=0;imd<mdtype.size();imd++){
									dcout << "       material: " << mdtype[imd].c_str() << std::endl;

									// Reading initial material stiffness tensor
									sprintf(filename, "%s/init.%s.stiff", macrostatelocin.c_str(), mdtype[imd].c_str());
									read_tensor<dim>(filename, stiffness_tensors[imd]);

									if(this_FE_process==0){
											std::cout << "          * stiffness: " << std::endl;
											printf("           %+.4e %+.4e %+.4e %+.4e %+.4e %+.4e \n",stiffness_tensors[imd][0][0][0][0], stiffness_tensors[imd][0][0][1][1], stiffness_tensors[imd][0][0][2][2], stiffness_tensors[imd][0][0][0][1], stiffness_tensors[imd][0][0][0][2], stiffness_tensors[imd][0][0][1][2]);
											printf("           %+.4e %+.4e %+.4e %+.4e %+.4e %+.4e \n",stiffness_tensors[imd][1][1][0][0], stiffness_tensors[imd][1][1][1][1], stiffness_tensors[imd][1][1][2][2], stiffness_tensors[imd][1][1][0][1], stiffness_tensors[imd][1][1][0][2], stiffness_tensors[imd][1][1][1][2]);
											printf("           %+.4e %+.4e %+.4e %+.4e %+.4e %+.4e \n",stiffness_tensors[imd][2][2][0][0], stiffness_tensors[imd][2][2][1][1], stiffness_tensors[imd][2][2][2][2], stiffness_tensors[imd][2][2][0][1], stiffness_tensors[imd][2][2][0][2], stiffness_tensors[imd][2][2][1][2]);
											printf("           %+.4e %+.4e %+.4e %+.4e %+.4e %+.4e \n",stiffness_tensors[imd][0][1][0][0], stiffness_tensors[imd][0][1][1][1], stiffness_tensors[imd][0][1][2][2], stiffness_tensors[imd][0][1][0][1], stiffness_tensors[imd][0][1][0][2], stiffness_tensors[imd][0][1][1][2]);
											printf("           %+.4e %+.4e %+.4e %+.4e %+.4e %+.4e \n",stiffness_tensors[imd][0][2][0][0], stiffness_tensors[imd][0][2][1][1], stiffness_tensors[imd][0][2][2][2], stiffness_tensors[imd][0][2][0][1], stiffness_tensors[imd][0][2][0][2], stiffness_tensors[imd][0][2][1][2]);
											printf("           %+.4e %+.4e %+.4e %+.4e %+.4e %+.4e \n",stiffness_tensors[imd][1][2][0][0], stiffness_tensors[imd][1][2][1][1], stiffness_tensors[imd][1][2][2][2], stiffness_tensors[imd][1][2][0][1], stiffness_tensors[imd][1][2][0][2], stiffness_tensors[imd][1][2][1][2]);
									}

									sprintf(filename, "%s/last.%s.stiff", macrostatelocout.c_str(), mdtype[imd].c_str());
									write_tensor<dim>(filename, stiffness_tensors[imd]);

									// Reading initial material density
									sprintf(filename, "%s/init.%s.density", macrostatelocin.c_str(), mdtype[imd].c_str());
									read_tensor<dim>(filename, densities[imd]);

									dcout << "          * density: " << densities[imd] << std::endl;

									sprintf(filename, "%s/last.%s.density", macrostatelocout.c_str(), mdtype[imd].c_str());
									write_tensor<dim>(filename, densities[imd]);


							}

							// Setting up distributed quadrature point local history
							unsigned int history_index = 0;
							for (typename Triangulation<dim>::active_cell_iterator
											cell = triangulation.begin_active();
											cell != triangulation.end(); ++cell)
									if (cell->is_locally_owned())
									{
											cell->set_user_pointer (&quadrature_point_history[history_index]);
											history_index += quadrature_formula.size();
									}

							Assert (history_index == quadrature_point_history.size(),
											ExcInternalError());

							// Create file with mdtype of qptid to update at timeid
							std::ofstream omatfile;
							char mat_local_filename[1024];
							sprintf(mat_local_filename, "%s/cell_id_mat.%d.list", macrostatelocout.c_str(), this_FE_process);
							omatfile.open (mat_local_filename);

							// Load the microstructure
							dcout << "    Loading microstructure..." << std::endl;
							celldata = get_microstructure();

							// Quadrature points data initialization and assigning material properties
							dcout << "    Assigning microstructure..." << std::endl;
							for (typename DoFHandler<dim>::active_cell_iterator
											cell = dof_handler.begin_active();
											cell != dof_handler.end(); ++cell)
									if (cell->is_locally_owned())
									{
											PointHistory<dim> *local_quadrature_points_history
													= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());
											Assert (local_quadrature_points_history >=
															&quadrature_point_history.front(),
															ExcInternalError());
											Assert (local_quadrature_points_history <
															&quadrature_point_history.back(),
															ExcInternalError());

											for (unsigned int q=0; q<quadrature_formula.size(); ++q)
											{
													local_quadrature_points_history[q].new_strain = 0;
													local_quadrature_points_history[q].upd_strain = 0;
													local_quadrature_points_history[q].new_stress = 0;
													local_quadrature_points_history[q].qpid = cell->active_cell_index()*quadrature_formula.size() + q;

													// Assign microstructure to the current cell (so far, mdtype
													// and rotation from global to common ground direction)

													if (q==0) assign_microstructure(cell, celldata,
																	local_quadrature_points_history[q].mat);
													else{
															local_quadrature_points_history[q].mat = local_quadrature_points_history[0].mat;
													}

													// Apply stiffness and rotating it from the local sheet orientation (MD) to
													// global orientation (microstructure)
													for (int imd = 0; imd<int(mdtype.size()); imd++)
															if(local_quadrature_points_history[q].mat==mdtype[imd]){
																	local_quadrature_points_history[q].new_stiff = stiffness_tensors[imd];

																	// Apply composite density (by averaging over replicas of given material)
																	local_quadrature_points_history[q].rho = densities[imd];
															}
													omatfile << local_quadrature_points_history[q].qpid << " " << local_quadrature_points_history[q].mat << std::endl;
											}
									}

							// Creating list of cell id/material mapping
							MPI_Barrier(FE_communicator);
							if (this_FE_process == 0){
									std::ifstream infile;
									std::ofstream outfile;
									std::string iline;

									sprintf(filename, "%s/cell_id_mat.list", macrostatelocout.c_str());
									outfile.open (filename);
									for (unsigned int ip=0; ip<n_FE_processes; ip++){
											char local_filename[1024];
											sprintf(local_filename, "%s/cell_id_mat.%d.list", macrostatelocout.c_str(), ip);
											infile.open (local_filename);
											while (getline(infile, iline)) outfile << iline << std::endl;
											infile.close();
											remove(local_filename);
									}
									outfile.close();
							}
							MPI_Barrier(FE_communicator);
					}



			template <int dim>
					void FEProblem<dim>::restart ()
					{
							char filename[1024];

							// Recovery of the solution vector containing total displacements in the
							// previous simulation and computing the total strain from it.
							sprintf(filename, "%s/restart/lcts.solution.bin", macrostatelocin.c_str());
							std::ifstream ifile(filename);
							if (ifile.is_open())
							{
									dcout << "    ...recovery of the position vector... " << std::flush;
									displacement.block_read(ifile);
									dcout << "    solution norm: " << displacement.l2_norm() << std::endl;
									ifile.close();

									dcout << "    ...computation of total strains from the recovered position vector. " << std::endl;
									FEValues<dim> fe_values (fe, quadrature_formula,
													update_values | update_gradients);
									std::vector<std::vector<Tensor<1,dim> > >
											solution_grads (quadrature_formula.size(),
															std::vector<Tensor<1,dim> >(dim));

									for (typename DoFHandler<dim>::active_cell_iterator
													cell = dof_handler.begin_active();
													cell != dof_handler.end(); ++cell)
											if (cell->is_locally_owned())
											{
													PointHistory<dim> *local_quadrature_points_history
															= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());
													Assert (local_quadrature_points_history >=
																	&quadrature_point_history.front(),
																	ExcInternalError());
													Assert (local_quadrature_points_history <
																	&quadrature_point_history.back(),
																	ExcInternalError());
													fe_values.reinit (cell);
													fe_values.get_function_gradients (displacement,
																	solution_grads);

													for (unsigned int q=0; q<quadrature_formula.size(); ++q)
													{
															// Strain tensor update
															local_quadrature_points_history[q].new_strain =
																	get_strain (solution_grads[q]);

															// Only needed if the mesh is modified after every timestep...
															/*const Tensor<2,dim> rotation
															  = get_rotation_matrix (solution_grads[q]);

															  const SymmetricTensor<2,dim> rotated_new_strain
															  = symmetrize(transpose(rotation) *
															  static_cast<Tensor<2,dim> >
															  (local_quadrature_points_history[q].new_strain) *
															  rotation);

															  local_quadrature_points_history[q].new_strain
															  = rotated_new_strain;*/
													}
											}
							}
							else{
									dcout << "    No file to load/restart displacements from." << std::endl;
							}

							// Recovery of the velocity vector
							sprintf(filename, "%s/restart/lcts.velocity.bin", macrostatelocin.c_str());
							std::ifstream ifile_veloc(filename);
							if (ifile_veloc.is_open())
							{
									dcout << "    ...recovery of the velocity vector... " << std::flush;
									velocity.block_read(ifile_veloc);
									dcout << "    velocity norm: " << velocity.l2_norm() << std::endl;
									ifile_veloc.close();
							}
							else{
									dcout << "    No file to load/restart velocities from." << std::endl;
							}

							// Opening processor local history file
							sprintf(filename, "%s/restart/lcts.pr_%d.lhistory.bin", macrostatelocin.c_str(), this_FE_process);
							std::ifstream  lhprocin(filename, std::ios_base::binary);

							// If openend, restore local data history...
							int ncell_lhistory=0;
							if (lhprocin.good()){
									std::string line;
									// Compute number of cells in local history ()
									while(getline(lhprocin, line)){
											//nline_lhistory++;
											// Extract values...
											std::istringstream sline(line);
											std::string var;
											int item_count = 0;
											int cell = 0;
											while(getline(sline, var, ',' )){
													if(item_count==1) cell = std::stoi(var);
													item_count++;
											}
											ncell_lhistory = std::max(ncell_lhistory, cell);
									}
									//int ncell_lhistory = n_FE_processes*nline_lhistory/quadrature_formula.size();
									//std::cout << "proc: " << this_FE_process << " ncell history: " << ncell_lhistory << std::endl;

									// Create structure to store retrieve data as matrix[cell][qpoint]
									std::vector<std::vector<PointHistory<dim>> > proc_lhistory (ncell_lhistory+1,
													std::vector<PointHistory<dim> >(quadrature_formula.size()));

									MPI_Barrier(FE_communicator);

									// Read and insert data
									lhprocin.clear();
									lhprocin.seekg(0, std::ios_base::beg);
									while(getline(lhprocin, line)){
											// Extract values...
											std::istringstream sline(line);
											std::string var;
											int item_count = 0;
											int cell = 0;
											int qpoint = 0;
											while(getline(sline, var, ',' )){
													if(item_count==1) cell = std::stoi(var);
													else if(item_count==2) qpoint = std::stoi(var);
													else if(item_count==4) proc_lhistory[cell][qpoint].upd_strain[0][0] = std::stod(var);
													else if(item_count==5) proc_lhistory[cell][qpoint].upd_strain[0][1] = std::stod(var);
													else if(item_count==6) proc_lhistory[cell][qpoint].upd_strain[0][2] = std::stod(var);
													else if(item_count==7) proc_lhistory[cell][qpoint].upd_strain[1][1] = std::stod(var);
													else if(item_count==8) proc_lhistory[cell][qpoint].upd_strain[1][2] = std::stod(var);
													else if(item_count==9) proc_lhistory[cell][qpoint].upd_strain[2][2] = std::stod(var);
													else if(item_count==10) proc_lhistory[cell][qpoint].new_stress[0][0] = std::stod(var);
													else if(item_count==11) proc_lhistory[cell][qpoint].new_stress[0][1] = std::stod(var);
													else if(item_count==12) proc_lhistory[cell][qpoint].new_stress[0][2] = std::stod(var);
													else if(item_count==13) proc_lhistory[cell][qpoint].new_stress[1][1] = std::stod(var);
													else if(item_count==14) proc_lhistory[cell][qpoint].new_stress[1][2] = std::stod(var);
													else if(item_count==15) proc_lhistory[cell][qpoint].new_stress[2][2] = std::stod(var);
													item_count++;
											}
											//				if(cell%90 == 0) std::cout << cell<<","<<qpoint<<","<<proc_lhistory[cell][qpoint].upd_strain[0][0]
											//				    <<","<<proc_lhistory[cell][qpoint].new_stress[0][0] << std::endl;
									}

									MPI_Barrier(FE_communicator);

									// Need to verify that the recovery of the local history is performed correctly...
									dcout << "    ...recovery of the quadrature point history. " << std::endl;
									for (typename DoFHandler<dim>::active_cell_iterator
													cell = dof_handler.begin_active();
													cell != dof_handler.end(); ++cell)
											if (cell->is_locally_owned())
											{
													PointHistory<dim> *local_quadrature_points_history
															= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());
													Assert (local_quadrature_points_history >=
																	&quadrature_point_history.front(),
																	ExcInternalError());
													Assert (local_quadrature_points_history <
																	&quadrature_point_history.back(),
																	ExcInternalError());

													for (unsigned int q=0; q<quadrature_formula.size(); ++q)
													{
															//std::cout << "proc: " << this_FE_process << " cell: " << cell->active_cell_index() << " qpoint: " << q << std::endl;
															// Assigning update strain and stress tensor
															local_quadrature_points_history[q].upd_strain=proc_lhistory[cell->active_cell_index()][q].upd_strain;
															local_quadrature_points_history[q].new_stress=proc_lhistory[cell->active_cell_index()][q].new_stress;
													}
											}
									lhprocin.close();
							}
							else{
									dcout << "    No file to load/restart local histories from." << std::endl;
							}
					}

			template <int dim>
					void FEProblem<dim>::set_boundary_values()
					{
						std::map<types::global_dof_index,double> boundary_values;

		        // define accelerations of boundary verticies, problem specific
						// e.g. defines acceleration of loaded verticies and sets edges to 0
						boundary_values = problem_type->set_boundary_conditions(timestep, fe_timestep_length);

						for (std::map<types::global_dof_index, double>::const_iterator
							p = boundary_values.begin();
							p != boundary_values.end(); ++p){
							incremental_velocity(p->first) = p->second;
				    }
					}

	

			template <int dim>
					double FEProblem<dim>::assemble_system (bool first_assemble)
					{
							double rhs_residual;

							typename DoFHandler<dim>::active_cell_iterator
									cell = dof_handler.begin_active(),
										 endc = dof_handler.end();

							FEValues<dim> fe_values (fe, quadrature_formula,
											update_values   | update_gradients |
											update_quadrature_points | update_JxW_values);

							const unsigned int   dofs_per_cell = fe.dofs_per_cell;
							const unsigned int   n_q_points    = quadrature_formula.size();

							FullMatrix<double>   cell_mass (dofs_per_cell, dofs_per_cell);
							Vector<double>       cell_force (dofs_per_cell);

							FullMatrix<double>   cell_v_matrix (dofs_per_cell, dofs_per_cell);
							Vector<double>       cell_v_rhs (dofs_per_cell);

							std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell);
		BodyForce<dim>      body_force;
		std::vector<Vector<double> > body_force_values (n_q_points,
				Vector<double>(dim));

		system_rhs = 0;
		system_matrix = 0;

		for (; cell!=endc; ++cell)
			if (cell->is_locally_owned())
			{
				cell_mass = 0;
				cell_force = 0;

				cell_v_matrix = 0;
				cell_v_rhs = 0;

				fe_values.reinit (cell);

				const PointHistory<dim> *local_quadrature_points_history
				= reinterpret_cast<PointHistory<dim>*>(cell->user_pointer());

				// Assembly of mass matrix
				if(first_assemble)
					for (unsigned int i=0; i<dofs_per_cell; ++i)
						for (unsigned int j=0; j<dofs_per_cell; ++j)
							for (unsigned int q_point=0; q_point<n_q_points;
									++q_point)
							{
								const double rho =
										local_quadrature_points_history[q_point].rho;

								const double
								phi_i = fe_values.shape_value (i,q_point),
								phi_j = fe_values.shape_value (j,q_point);

								// Non-zero value only if same dimension DOF, because
								// this is normally a scalar product of the shape functions vector
								int dcorr;
								if(i%dim==j%dim) dcorr = 1;
								else dcorr = 0;

								// Lumped mass matrix because the consistent one doesnt work...
								cell_mass(i,i) // cell_mass(i,j) instead...
								+= (rho * dcorr * phi_i * phi_j
										* fe_values.JxW (q_point));
							}

				// Assembly of external forces vector
				for (unsigned int i=0; i<dofs_per_cell; ++i)
				{
					const unsigned int
					component_i = fe.system_to_component_index(i).first;

					for (unsigned int q_point=0; q_point<n_q_points; ++q_point)
					{
						body_force.vector_value_list (fe_values.get_quadrature_points(),
								body_force_values);

						const SymmetricTensor<2,dim> &new_stress
						= local_quadrature_points_history[q_point].new_stress;

						// how to handle body forces?
						cell_force(i) += (
								body_force_values[q_point](component_i) *
								local_quadrature_points_history[q_point].rho *
								fe_values.shape_value (i,q_point)
								-
								new_stress *
								get_strain (fe_values,i,q_point))
								*
								fe_values.JxW (q_point);
					}
				}

				cell->get_dof_indices (local_dof_indices);

				// Assemble local matrices for v problem
				if(first_assemble) cell_v_matrix = cell_mass;

				//std::cout << "norm matrix " << cell_v_matrix.l1_norm() << " stiffness " << cell_stiffness.l1_norm() << std::endl;

				// Assemble local rhs for v problem
				cell_v_rhs.add(fe_timestep_length, cell_force);

				// Local to global for u and v problems
				if(first_assemble) hanging_node_constraints
										.distribute_local_to_global(cell_v_matrix, cell_v_rhs,
												local_dof_indices,
												system_matrix, system_rhs);
				else hanging_node_constraints
						.distribute_local_to_global(cell_v_rhs,
								local_dof_indices, system_rhs);
			}

		if(first_assemble){
			system_matrix.compress(VectorOperation::add);
			mass_matrix.copy_from(system_matrix);
		}
		else system_matrix.copy_from(mass_matrix);

		system_rhs.compress(VectorOperation::add);


		FEValuesExtractors::Scalar x_component (dim-3);
		FEValuesExtractors::Scalar y_component (dim-2);
		FEValuesExtractors::Scalar z_component (dim-1);

		std::map<types::global_dof_index,double> boundary_values;
		boundary_values = problem_type->boundary_conditions_to_zero(timestep);

		PETScWrappers::MPI::Vector tmp (locally_owned_dofs,FE_communicator);
		MatrixTools::apply_boundary_values (boundary_values,
				system_matrix,
				tmp,
				system_rhs,
				false);
		newton_update_velocity = tmp;

		rhs_residual = system_rhs.l2_norm();
		dcout << "    FE System - norm of rhs is " << rhs_residual
							  << std::endl;

		return rhs_residual;
	}



	template <int dim>
	void FEProblem<dim>::solve_linear_problem_CG ()
	{
		PETScWrappers::MPI::Vector
		distributed_newton_update (locally_owned_dofs,FE_communicator);
		distributed_newton_update = newton_update_velocity;

		// The residual used internally to test solver convergence is
		// not identical to ours, it probably considers preconditionning.
		// Therefore, extra precision is required in the solver proportionnaly
		// to the norm of the system matrix, to reduce sufficiently our residual
		SolverControl       solver_control (dof_handler.n_dofs(),
				1e-03);

		PETScWrappers::SolverCG cg (solver_control,
				FE_communicator);

		// Apparently (according to step-17.tuto) the BlockJacobi preconditionner is
		// not optimal for large scale simulations.
		PETScWrappers::PreconditionJacobi preconditioner(system_matrix);
		cg.solve (system_matrix, distributed_newton_update, system_rhs,
				preconditioner);

		newton_update_velocity = distributed_newton_update;
		hanging_node_constraints.distribute (newton_update_velocity);

		dcout << "    FE Solver - norm of newton update is " << newton_update_velocity.l2_norm()
							  << std::endl;
		dcout << "    FE Solver converged in " << solver_control.last_step()
				<< " iterations "
				<< " with value " << solver_control.last_value()
				<<  std::endl;
	}



	template <int dim>
	void FEProblem<dim>::solve_linear_problem_GMRES ()
	{
		PETScWrappers::MPI::Vector
		distributed_newton_update (locally_owned_dofs,FE_communicator);
		distributed_newton_update = newton_update_velocity;

		// The residual used internally to test solver convergence is
		// not identical to ours, it probably considers preconditionning.
		// Therefore, extra precision is required in the solver proportionnaly
		// to the norm of the system matrix, to reduce sufficiently our residual
		SolverControl       solver_control (dof_handler.n_dofs(),
				1e-03);

		PETScWrappers::SolverGMRES gmres (solver_control,
				FE_communicator);

		// Apparently (according to step-17.tuto) the BlockJacobi preconditionner is
		// not optimal for large scale simulations.
		PETScWrappers::PreconditionBlockJacobi preconditioner(system_matrix);
		gmres.solve (system_matrix, distributed_newton_update, system_rhs,
				preconditioner);

		newton_update_velocity = distributed_newton_update;
		hanging_node_constraints.distribute (newton_update_velocity);

		dcout << "    FE Solver - norm of newton update is " << newton_update_velocity.l2_norm()
							  << std::endl;
		dcout << "    FE Solver converged in " << solver_control.last_step()
				<< " iterations "
				<< " with value " << solver_control.last_value()
				<<  std::endl;
	}



	template <int dim>
	void FEProblem<dim>::solve_linear_problem_BiCGStab ()
	{
		PETScWrappers::MPI::Vector
		distributed_newton_update (locally_owned_dofs,FE_communicator);
		distributed_newton_update = newton_update_velocity;

		PETScWrappers::PreconditionBoomerAMG preconditioner;
		  {
		    PETScWrappers::PreconditionBoomerAMG::AdditionalData additional_data;
		    additional_data.symmetric_operator = true;

		    preconditioner.initialize(system_matrix, additional_data);
		  }

		// The residual used internally to test solver convergence is
		// not identical to ours, it probably considers preconditionning.
		// Therefore, extra precision is required in the solver proportionnaly
		// to the norm of the system matrix, to reduce sufficiently our residual
		SolverControl       solver_control (dof_handler.n_dofs(),
				1e-03);

		PETScWrappers::SolverBicgstab bicgs (solver_control,
				FE_communicator);

		bicgs.solve (system_matrix, distributed_newton_update, system_rhs,
				preconditioner);

		newton_update_velocity = distributed_newton_update;
		hanging_node_constraints.distribute (newton_update_velocity);

		dcout << "    FE Solver - norm of newton update is " << newton_update_velocity.l2_norm()
							  << std::endl;
		dcout << "    FE Solver converged in " << solver_control.last_step()
				<< " iterations "
				<< " with value " << solver_control.last_value()
				<<  std::endl;
	}



	template <int dim>
	void FEProblem<dim>::solve_linear_problem_direct ()
	{
		PETScWrappers::MPI::Vector
		distributed_newton_update (locally_owned_dofs,FE_communicator);
		distributed_newton_update = newton_update_velocity;

		SolverControl       solver_control;

		PETScWrappers::SparseDirectMUMPS solver (solver_control,
				FE_communicator);

		//solver.set_symmetric_mode(false);

		solver.solve (system_matrix, distributed_newton_update, system_rhs);
		//system_inverse.vmult(distributed_newton_update, system_rhs);

		newton_update_velocity = distributed_newton_update;
		hanging_node_constraints.distribute (newton_update_velocity);

		dcout << "    FE Solver - norm of newton update is " << newton_update_velocity.l2_norm()
							  << std::endl;
	}



	template <int dim>
	void FEProblem<dim>::update_incremental_variables ()
	{
		// Displacement newton update is equal to the current velocity multiplied by the timestep length
		newton_update_displacement.equ(fe_timestep_length, velocity);
		newton_update_displacement.add(fe_timestep_length, incremental_velocity);
		newton_update_displacement.add(fe_timestep_length, newton_update_velocity);
		newton_update_displacement.add(-1.0, incremental_displacement);

		//hcout << "    Upd. Norms: " << fe_problem.newton_update_displacement.l2_norm() << " - " << fe_problem.newton_update_velocity.l2_norm() <<  std::endl;

		//fe_problem.newton_update_displacement.equ(fe_timestep_length, fe_problem.newton_update_velocity);

		incremental_velocity.add (1.0, newton_update_velocity);
		incremental_displacement.add (1.0, newton_update_displacement);
		//hcout << "    Inc. Norms: " << fe_problem.incremental_displacement.l2_norm() << " - " << fe_problem.incremental_velocity.l2_norm() <<  std::endl;
	}




	template <int dim>
	void FEProblem<dim>::update_strain_quadrature_point_history(const Vector<double>& displacement_update)
	{
		// Preparing requirements for strain update
		FEValues<dim> fe_values (fe, quadrature_formula,
				update_values | update_gradients);
		std::vector<std::vector<Tensor<1,dim> > >
		displacement_update_grads (quadrature_formula.size(),
				std::vector<Tensor<1,dim> >(dim));

		for (typename DoFHandler<dim>::active_cell_iterator
				cell = dof_handler.begin_active();
				cell != dof_handler.end(); ++cell)
			if (cell->is_locally_owned())
			{
				SymmetricTensor<2,dim> newton_strain_tensor;

				PointHistory<dim> *local_quadrature_points_history
				= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());
				Assert (local_quadrature_points_history >=
						&quadrature_point_history.front(),
						ExcInternalError());
				Assert (local_quadrature_points_history <
						&quadrature_point_history.back(),
						ExcInternalError());
				fe_values.reinit (cell);
				fe_values.get_function_gradients (displacement_update,
						displacement_update_grads);

				for (unsigned int q=0; q<quadrature_formula.size(); ++q)
				{
					local_quadrature_points_history[q].old_strain =
							local_quadrature_points_history[q].new_strain;

					local_quadrature_points_history[q].old_stress =
							local_quadrature_points_history[q].new_stress;

					local_quadrature_points_history[q].old_stiff =
							local_quadrature_points_history[q].new_stiff;

					if (newtonstep == 0) local_quadrature_points_history[q].inc_strain = 0.;

					// Strain tensor update
					local_quadrature_points_history[q].newton_strain = get_strain (displacement_update_grads[q]);
					local_quadrature_points_history[q].inc_strain += local_quadrature_points_history[q].newton_strain;
					local_quadrature_points_history[q].new_strain += local_quadrature_points_history[q].newton_strain;
					local_quadrature_points_history[q].upd_strain += local_quadrature_points_history[q].newton_strain;
				}
			}
	}




	template <int dim>
	void FEProblem<dim>::write_qp_update_list(ScaleBridgingData &scale_bridging_data)
	{
		std::vector<int> qpupdates;
		std::vector<double> strains;

		std::vector<SymmetricTensor<2,dim> > update_strains;

		for (typename DoFHandler<dim>::active_cell_iterator
				cell = dof_handler.begin_active();
				cell != dof_handler.end(); ++cell)
			if (cell->is_locally_owned())
			{
				PointHistory<dim> *local_quadrature_points_history
				= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());
				Assert (local_quadrature_points_history >=
						&quadrature_point_history.front(),
						ExcInternalError());
				Assert (local_quadrature_points_history <
						&quadrature_point_history.back(),
						ExcInternalError());
				for (unsigned int q=0; q<quadrature_formula.size(); ++q)
				{
					// The cell will get its stress from MD, but should it run an MD simulation?
					if (true
							// in case of extreme straining with reaxff
							/*&& !(avg_new_stress_tensor.norm() < 1.0e8 && avg_new_strain_tensor.norm() > 3.0)*/
					){

						QP qp; // Struct that holds information for md job

						for (int i=0; i<6; i++){
							qp.update_strain[i] = local_quadrature_points_history[q].upd_strain.access_raw_entry(i);
							// Just in case we do not update the stress more precisely (e.g. with MD or a surrogate model) (Maxime)
							SymmetricTensor<2,dim> tmp_stress = local_quadrature_points_history[q].new_stiff
									*local_quadrature_points_history[q].new_strain;
							qp.update_stress[i] = tmp_stress.access_raw_entry(i);
						}
						qp.id = local_quadrature_points_history[q].qpid;
						qp.material = celldata.get_composition(cell->active_cell_index());
						scale_bridging_data.update_list.push_back(qp);
						//sprintf(filename, "%s/last.%s.upstrain", macrostatelocout.c_str(), cell_id);
						//write_tensor<dim>(filename, rot_avg_upd_strain_tensor);

						// qpupdates.push_back(local_quadrature_points_history[q].qpid); //MPI list of qps to update on this rank
						//std::cout<< "local qpid "<< local_quadrature_points_history[q].qpid << std::endl;
					}
				}
			}
		// Gathering in a single file all the quadrature points to be updated...
		// Might be worth replacing indivual local file writings by a parallel vector of string
		// and globalizing this vector before this final writing step.
		gather_qp_update_list(scale_bridging_data);

		//std::vector<int> all_qpupdates;
		///all_qpupdates = gather_vector<int>(qpupdates);
		/*for (int i=0; i < all_qpupdates.size(); i++){
			QP qp;
			qp.id = all_qpupdates[i];
			qp.material = celldata.get_composition(qp.id);
			scale_bridging_data.update_list.push_back(qp);		
		}*/
	}

	template <int dim>
	void FEProblem<dim>::gather_qp_update_list(ScaleBridgingData &scale_bridging_data)
	{
		scale_bridging_data.update_list = gather_vector<QP>(scale_bridging_data.update_list);
	}
			
	template <int dim>
	template <typename T>
	std::vector<T> FEProblem<dim>::gather_vector(std::vector<T> local_vector)
	{
		// Gather a variable length vector held on each rank into one vector on rank 0
		int elements_on_this_proc = local_vector.size();
		std::vector<int> elements_per_proc(n_FE_processes); // number of elements on each rank
		MPI_Gather(&elements_on_this_proc, 	//sendbuf
				1,														//sendcount
				MPI_INT,											//sendtype
				&elements_per_proc.front(),		//recvbuf
				1,														//rcvcount
				MPI_INT,											//recvtype
				0,
				FE_communicator);

		uint32_t total_elements = 0; 
		for (uint32_t i = 0; i < n_FE_processes; i++)
		{
			total_elements += elements_per_proc[i];
		}

		// Displacement local vector in the main vector for use in MPI_Gatherv
		int *disps = new int[n_FE_processes];
		for (int i = 0; i < n_FE_processes; i++)
		{
			disps[i] = (i > 0) ? (disps[i-1] + elements_per_proc[i-1]) : 0;
		}

		std::vector<T> gathered_vector(total_elements);
		if      (typeid(T) == typeid(int)){
			MPI_Gatherv(&local_vector.front(),
					local_vector.size(),
					MPI_INT,
					&gathered_vector.front(),
					&elements_per_proc.front(),
					disps, MPI_INT, 0, FE_communicator);
		}

		else if (typeid(T) == typeid(double)){
			MPI_Gatherv(&local_vector.front(),
					local_vector.size(),
					MPI_DOUBLE,
					&gathered_vector.front(),
					&elements_per_proc.front(),
					disps, MPI_DOUBLE, 0, FE_communicator);
		}

		else if (typeid(T) == typeid(QP)){
			MPI_Gatherv(&local_vector.front(),
					local_vector.size(),
					MPI_QP,
					&gathered_vector.front(),
					&elements_per_proc.front(),
					disps, MPI_QP, 0, FE_communicator);
		}
		else {
			dcout<<"Type not implemented in gather_vector"<<std::endl;
			exit(1);
		}

		// Populate a list with all elements requested
		// TODO This is the general function, some day we will make this work with generic type
		/*std::vector<T> gathered_vector(total_elements); // vector with all elements from all ranks
		MPI_Gatherv(&local_vector.front(),     // *sendbuf,
            local_vector.size(),       	// sendcount,
            mpi_type,										// sendtype,
  					&gathered_vector.front(),	    // *recvbuf,
  					&elements_per_proc.front(),	// *recvcounts[],
						disps,											// displs[],
            mpi_type, 										//recvtype,
						0,
						FE_communicator);*/


		/*		
		if (this_FE_process == 0){
			dcout << "GATHER VECTOR OUTPUT " << total_elements << " ";
			for (int i = 0; i < gathered_vector.size(); i++)
			{
				dcout << gathered_vector[i] << " " ;
			}
			dcout << std::endl;
		}*/

		return gathered_vector;
	}

	QP get_qp_with_id (int qp_id, ScaleBridgingData scale_bridging_data)
	{
		QP qp; 
		bool found = false;
		int n_qp = scale_bridging_data.update_list.size();

		for (int i=0; i<n_qp; i++){
			if (scale_bridging_data.update_list[i].id == qp_id){
				qp = scale_bridging_data.update_list[i];
				found = true;
				break;
			}
		}
		if (found == false){
			std::cout << "Error: No QP object with id "<< qp_id << std::endl;
			exit(1);
		}
		return qp;
	}

	template <int dim>
	void FEProblem<dim>::update_stress_quadrature_point_history(ScaleBridgingData scale_bridging_data)
	{
		// Retrieving all quadrature points computation and storing them in the
		// quadrature_points_history structure
		for (typename DoFHandler<dim>::active_cell_iterator
				cell = dof_handler.begin_active();
				cell != dof_handler.end(); ++cell){
			if (cell->is_locally_owned())
			{

				PointHistory<dim> *local_quadrature_points_history
				= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());
				Assert (local_quadrature_points_history >=
						&quadrature_point_history.front(),
						ExcInternalError());
				Assert (local_quadrature_points_history <
						&quadrature_point_history.back(),
						ExcInternalError());

				for (unsigned int q=0; q<quadrature_formula.size(); ++q)
				{
					int qp_id = local_quadrature_points_history[q].qpid;

					if (newtonstep == 0) local_quadrature_points_history[q].inc_stress = 0.;

					QP qp;
					qp = get_qp_with_id(qp_id, scale_bridging_data);

					SymmetricTensor<2,dim> loc_stress(qp.update_stress);

					local_quadrature_points_history[q].new_stress = loc_stress;

					// Resetting the update strain tensor
					local_quadrature_points_history[q].upd_strain = 0;
				}
			}
		}
	}




	template <int dim>
	Vector<double> FEProblem<dim>::compute_internal_forces () const
	{
		PETScWrappers::MPI::Vector residual
		(locally_owned_dofs, FE_communicator);

		residual = 0;

		FEValues<dim> fe_values (fe, quadrature_formula,
				update_values   | update_gradients |
				update_quadrature_points | update_JxW_values);

		const unsigned int   dofs_per_cell = fe.dofs_per_cell;
		const unsigned int   n_q_points    = quadrature_formula.size();

		Vector<double>               cell_residual (dofs_per_cell);

		std::vector<types::global_dof_index> local_dof_indices (dofs_per_cell);

		typename DoFHandler<dim>::active_cell_iterator
		cell = dof_handler.begin_active(),
		endc = dof_handler.end();
		for (; cell!=endc; ++cell)
			if (cell->is_locally_owned())
			{
				cell_residual = 0;
				fe_values.reinit (cell);

				const PointHistory<dim> *local_quadrature_points_history
				= reinterpret_cast<PointHistory<dim>*>(cell->user_pointer());

				for (unsigned int i=0; i<dofs_per_cell; ++i)
				{
					for (unsigned int q_point=0; q_point<n_q_points; ++q_point)
					{
						const SymmetricTensor<2,dim> &old_stress
						= local_quadrature_points_history[q_point].new_stress;

						cell_residual(i) +=
								(old_stress *
								get_strain (fe_values,i,q_point))
								*
								fe_values.JxW (q_point);
					}
				}

				cell->get_dof_indices (local_dof_indices);
				hanging_node_constraints.distribute_local_to_global
				(cell_residual, local_dof_indices, residual);
			}

		residual.compress(VectorOperation::add);

		Vector<double> local_residual (dof_handler.n_dofs());
		local_residual = residual;

		return local_residual;
	}




	template <int dim>
	std::vector< std::vector< Vector<double> > >
	FEProblem<dim>::compute_history_projection_from_qp_to_nodes (FE_DGQ<dim> &history_fe, DoFHandler<dim> &history_dof_handler, std::string stensor) const
	{
		std::vector< std::vector< Vector<double> > >
		             history_field (dim, std::vector< Vector<double> >(dim)),
		             local_history_values_at_qpoints (dim, std::vector< Vector<double> >(dim)),
		             local_history_fe_values (dim, std::vector< Vector<double> >(dim));
		for (unsigned int i=0; i<dim; i++)
		  for (unsigned int j=0; j<dim; j++)
		  {
		    history_field[i][j].reinit(history_dof_handler.n_dofs());
		    local_history_values_at_qpoints[i][j].reinit(quadrature_formula.size());
		    local_history_fe_values[i][j].reinit(history_fe.dofs_per_cell);
		  }
		FullMatrix<double> qpoint_to_dof_matrix (history_fe.dofs_per_cell,
		                                         quadrature_formula.size());
		FETools::compute_projection_from_quadrature_points_matrix
		          (history_fe,
		           quadrature_formula, quadrature_formula,
		           qpoint_to_dof_matrix);
		typename DoFHandler<dim>::active_cell_iterator cell = dof_handler.begin_active(),
		                                               endc = dof_handler.end(),
		                                               dg_cell = history_dof_handler.begin_active();
		for (; cell!=endc; ++cell, ++dg_cell){
			if (cell->is_locally_owned()){
				PointHistory<dim> *local_quadrature_points_history
				= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());
				Assert (local_quadrature_points_history >=
						&quadrature_point_history.front(),
						ExcInternalError());
				Assert (local_quadrature_points_history <
						&quadrature_point_history.back(),
						ExcInternalError());
				for (unsigned int i=0; i<dim; i++){
					for (unsigned int j=0; j<dim; j++)
					{
						for (unsigned int q=0; q<quadrature_formula.size(); ++q){
							if (stensor == "strain"){
								local_history_values_at_qpoints[i][j](q)
				                		   = local_quadrature_points_history[q].new_strain[i][j];
							}
							else if(stensor == "stress"){
								local_history_values_at_qpoints[i][j](q)
				                		   = local_quadrature_points_history[q].new_stress[i][j];
							}
							else{
								std::cerr << "Error: Neither 'stress' nor 'strain' to be projected to DOFs..." << std::endl;
							}
						}
						qpoint_to_dof_matrix.vmult (local_history_fe_values[i][j],
								local_history_values_at_qpoints[i][j]);
						dg_cell->set_dof_values (local_history_fe_values[i][j],
								history_field[i][j]);
					}
				}
			}
			else{
				for (unsigned int i=0; i<dim; i++){
					for (unsigned int j=0; j<dim; j++)
					{
						for (unsigned int q=0; q<quadrature_formula.size(); ++q){
							local_history_values_at_qpoints[i][j](q) = -1e+20;
						}
						qpoint_to_dof_matrix.vmult (local_history_fe_values[i][j],
								local_history_values_at_qpoints[i][j]);
						dg_cell->set_dof_values (local_history_fe_values[i][j],
								history_field[i][j]);
					}
				}
			}
		}

		return history_field;
	}



    template <int dim>
    void FEProblem<dim>::output_lbc_force ()
    {
            // Compute applied force vector
            Vector<double> local_residual (dof_handler.n_dofs());
            local_residual = compute_internal_forces(); // Note that local_residual contains the local force at all nodes of the mesh on all ranks

            // Write specific outputs to file
            if (this_FE_process==0)
            {
           	 	// Compute force under the loading boundary condition
           	 	double aforce = 0.;
           	 	for (unsigned int i=0; i<dof_handler.n_dofs(); ++i)
           	 	        if (problem_type->is_vertex_loaded(i) == true)
           	 	        {
           	 	                aforce += local_residual[i];
           	 	        }

                    std::ofstream ofile;
                    char fname[1024]; sprintf(fname, "%s/loadedbc_force.csv", macrologloc.c_str());

                    // writing the header of the file
                    if (timestep == start_timestep){
                            ofile.open (fname);
                            if (ofile.is_open()){
                                    ofile << "timestep,time,resulting_force" << std::endl;
                                    ofile.close();
                            }
                            else std::cout << "Unable to open" << fname << " to write in it" << std::endl;
                    }

                    ofile.open (fname, std::ios::app);
                    if (ofile.is_open())
                    {
                            ofile << timestep << ", " << present_time << ", " << aforce << std::endl;
                            ofile.close();
                    }
                    else std::cout << "Unable to open" << fname << " to write in it" << std::endl;
            }
    }



	template <int dim>
	void FEProblem<dim>::output_lhistory ()
	{
		char filename[1024];

		// Initialization of the processor local history data file
		sprintf(filename, "%s/pr_%d.lhistory.csv", macrologloc.c_str(), this_FE_process);
		std::ofstream  lhprocout(filename, std::ios_base::app);
		long cursor_position = lhprocout.tellp();

		if (cursor_position == 0)
		{
			lhprocout << "timestep,time,qpid,cell,qpoint,material";
			for(unsigned int k=0;k<dim;k++)
				for(unsigned int l=k;l<dim;l++)
					lhprocout << "," << "strain_" << k << l;
			for(unsigned int k=0;k<dim;k++)
				for(unsigned int l=k;l<dim;l++)
					lhprocout  << "," << "updstrain_" << k << l;
			for(unsigned int k=0;k<dim;k++)
				for(unsigned int l=k;l<dim;l++)
					lhprocout << "," << "stress_" << k << l;
			lhprocout << std::endl;
		}

		// Output of complete local history in a single file per processor
		for (typename DoFHandler<dim>::active_cell_iterator
				cell = dof_handler.begin_active();
				cell != dof_handler.end(); ++cell)
			if (cell->is_locally_owned())
			{
				char cell_id[1024]; sprintf(cell_id, "%d", cell->active_cell_index());

				PointHistory<dim> *local_qp_hist
				= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());

				// Save strain, updstrain, stress history in one file per proc
				for (unsigned int q=0; q<quadrature_formula.size(); ++q)
				{
					lhprocout << timestep
							<< "," << present_time
							<< "," << local_qp_hist[q].qpid
							<< "," << cell->active_cell_index()
							<< "," << q
							<< "," << local_qp_hist[q].mat.c_str();
					for(unsigned int k=0;k<dim;k++)
						for(unsigned int l=k;l<dim;l++){
							lhprocout << "," << std::setprecision(16) << local_qp_hist[q].new_strain[k][l];
						}
					for(unsigned int k=0;k<dim;k++)
						for(unsigned int l=k;l<dim;l++){
							lhprocout << "," << std::setprecision(16) << local_qp_hist[q].upd_strain[k][l];
						}
					for(unsigned int k=0;k<dim;k++)
						for(unsigned int l=k;l<dim;l++){
							lhprocout << "," << std::setprecision(16) << local_qp_hist[q].new_stress[k][l];
						}
					lhprocout << std::endl;
				}
			}
		lhprocout.close();
	}




	template <int dim>
	void FEProblem<dim>::output_visualisation_history ()
	{
		// Data structure for VTK output
		DataOut<dim> data_out;
		data_out.attach_dof_handler (history_dof_handler);

		// Output of the cell norm of the averaged strain tensor over quadrature
		// points as a scalar
		std::vector<std::string> stensor_proj;
		stensor_proj.push_back("strain");
		stensor_proj.push_back("stress");

		std::vector<std::vector< std::vector< Vector<double> > > > tensor_proj;

		for(unsigned int i=0; i<stensor_proj.size(); i++){
			tensor_proj.push_back(compute_history_projection_from_qp_to_nodes (history_fe, history_dof_handler, stensor_proj[i]));
			data_out.add_data_vector (tensor_proj[i][0][0], stensor_proj[i]+"_xx");
			data_out.add_data_vector (tensor_proj[i][1][1], stensor_proj[i]+"_yy");
			data_out.add_data_vector (tensor_proj[i][2][2], stensor_proj[i]+"_zz");
			data_out.add_data_vector (tensor_proj[i][0][1], stensor_proj[i]+"_xy");
			data_out.add_data_vector (tensor_proj[i][0][2], stensor_proj[i]+"_xz");
			data_out.add_data_vector (tensor_proj[i][1][2], stensor_proj[i]+"_yz");
		}

		data_out.build_patches ();

		// Grouping spatially partitioned outputs
		std::string filename = macrologloc + "/" + "history-" + Utilities::int_to_string(timestep,4)
		+ "." + Utilities::int_to_string(this_FE_process,3)
		+ ".vtu";
		AssertThrow (n_FE_processes < 1000, ExcNotImplemented());

		std::ofstream output (filename.c_str());
		data_out.write_vtu (output);

		MPI_Barrier(FE_communicator); // just to be safe
		if (this_FE_process==0)
		{
			std::vector<std::string> filenames_loc;
			for (unsigned int i=0; i<n_FE_processes; ++i)
				filenames_loc.push_back ("history-" + Utilities::int_to_string(timestep,4)
			+ "." + Utilities::int_to_string(i,3)
			+ ".vtu");

			const std::string
			visit_master_filename = (macrologloc + "/" + "history-" +
					Utilities::int_to_string(timestep,4) +
					".visit");
			std::ofstream visit_master (visit_master_filename.c_str());
			//data_out.write_visit_record (visit_master, filenames_loc); // 8.4.1
			DataOutBase::write_visit_record (visit_master, filenames_loc); // 8.5.0

			const std::string
			pvtu_master_filename = (macrologloc + "/" + "history-" +
					Utilities::int_to_string(timestep,4) +
					".pvtu");
			std::ofstream pvtu_master (pvtu_master_filename.c_str());
			data_out.write_pvtu_record (pvtu_master, filenames_loc);

			static std::vector<std::pair<double,std::string> > times_and_names;
			const std::string
						pvtu_master_filename_loc = ("history-" +
								Utilities::int_to_string(timestep,4) +
								".pvtu");
			times_and_names.push_back (std::pair<double,std::string> (present_time, pvtu_master_filename_loc));
			std::ofstream pvd_output (macrologloc + "/" + "history.pvd");
			//data_out.write_pvd_record (pvd_output, times_and_names); // 8.4.1
			DataOutBase::write_pvd_record (pvd_output, times_and_names); // 8.5.0
		}
		MPI_Barrier(FE_communicator);
	}




	template <int dim>
	void FEProblem<dim>::output_visualisation_solution ()
	{
		// Data structure for VTK output
		DataOut<dim> data_out;
		data_out.attach_dof_handler (dof_handler);

		// Output of displacement as a vector
		std::vector<DataComponentInterpretation::DataComponentInterpretation>
		data_component_interpretation
		(dim, DataComponentInterpretation::component_is_part_of_vector);
		std::vector<std::string>  displacement_names (dim, "displacement");
		data_out.add_data_vector (displacement,
				displacement_names,
				DataOut<dim>::type_dof_data,
				data_component_interpretation);

		// Output of velocity as a vector
		std::vector<std::string>  velocity_names (dim, "velocity");
		data_out.add_data_vector (velocity,
				velocity_names,
				DataOut<dim>::type_dof_data,
				data_component_interpretation);

		// Output of internal forces as a vector
		Vector<double> fint = compute_internal_forces ();
		std::vector<std::string>  fint_names (dim, "fint");
		data_out.add_data_vector (fint,
				fint_names,
				DataOut<dim>::type_dof_data,
				data_component_interpretation);

		// Output of the cell averaged striffness over quadrature
		// points as a scalar in direction 0000, 1111 and 2222
		std::vector<Vector<double> > avg_stiff (dim,
				Vector<double>(triangulation.n_active_cells()));
		for (int i=0;i<dim;++i){
			{
				typename Triangulation<dim>::active_cell_iterator
				cell = triangulation.begin_active(),
				endc = triangulation.end();
				for (; cell!=endc; ++cell)
					if (cell->is_locally_owned())
					{
						double accumulated_stiffi = 0.;
						for (unsigned int q=0;q<quadrature_formula.size();++q)
							accumulated_stiffi += reinterpret_cast<PointHistory<dim>*>
								(cell->user_pointer())[q].new_stiff[i][i][i][i];

						avg_stiff[i](cell->active_cell_index()) = accumulated_stiffi/quadrature_formula.size();
					}
					else avg_stiff[i](cell->active_cell_index()) = -1e+20;
			}
			std::string si = std::to_string(i);
			std::string name = "stiffness_"+si+si+si+si;
			data_out.add_data_vector (avg_stiff[i], name);
		}


		// Output of the cell id
		Vector<double> cell_ids (triangulation.n_active_cells());
		{
			typename Triangulation<dim>::active_cell_iterator
			cell = triangulation.begin_active(),
			endc = triangulation.end();
			for (; cell!=endc; ++cell)
				if (cell->is_locally_owned())
				{
					cell_ids(cell->active_cell_index())
							= cell->active_cell_index();
				}
				else cell_ids(cell->active_cell_index()) = -1;
		}
		data_out.add_data_vector (cell_ids, "cellID");

		// Output of the partitioning of the mesh on processors
		std::vector<types::subdomain_id> partition_int (triangulation.n_active_cells());
		GridTools::get_subdomain_association (triangulation, partition_int);
		const Vector<double> partitioning(partition_int.begin(),
				partition_int.end());
		data_out.add_data_vector (partitioning, "partitioning");

		data_out.build_patches ();

		// Grouping spatially partitioned outputs
		std::string filename = macrologloc + "/" + "solution-" + Utilities::int_to_string(timestep,4)
		+ "." + Utilities::int_to_string(this_FE_process,3)
		+ ".vtu";
		AssertThrow (n_FE_processes < 1000, ExcNotImplemented());

		std::ofstream output (filename.c_str());
		data_out.write_vtu (output);

		MPI_Barrier(FE_communicator); // just to be safe
		if (this_FE_process==0)
		{
			std::vector<std::string> filenames_loc;
			for (unsigned int i=0; i<n_FE_processes; ++i)
				filenames_loc.push_back ("solution-" + Utilities::int_to_string(timestep,4)
			+ "." + Utilities::int_to_string(i,3)
			+ ".vtu");

			const std::string
			visit_master_filename = (macrologloc + "/" + "solution-" +
					Utilities::int_to_string(timestep,4) +
					".visit");
			std::ofstream visit_master (visit_master_filename.c_str());
			//data_out.write_visit_record (visit_master, filenames_loc); // 8.4.1
			DataOutBase::write_visit_record (visit_master, filenames_loc); // 8.5.0

			const std::string
			pvtu_master_filename = (macrologloc + "/" + "solution-" +
					Utilities::int_to_string(timestep,4) +
					".pvtu");
			std::ofstream pvtu_master (pvtu_master_filename.c_str());
			data_out.write_pvtu_record (pvtu_master, filenames_loc);

			static std::vector<std::pair<double,std::string> > times_and_names;
			const std::string
						pvtu_master_filename_loc = ("solution-" +
								Utilities::int_to_string(timestep,4) +
								".pvtu");
			times_and_names.push_back (std::pair<double,std::string> (present_time, pvtu_master_filename_loc));
			std::ofstream pvd_output (macrologloc + "/" + "solution.pvd");
			//data_out.write_pvd_record (pvd_output, times_and_names); // 8.4.1
			DataOutBase::write_pvd_record (pvd_output, times_and_names); // 8.5.0
		}
		MPI_Barrier(FE_communicator);
	}



	template <int dim>
	void FEProblem<dim>::output_results ()
	{
		// Output resulting force at boundary
		if(timestep%freq_output_lbcforce==0) output_lbc_force();

		// Output local history by processor
		if(timestep%freq_output_lhist==0) output_lhistory ();

		// Output visualisation files for paraview
		if(timestep%freq_output_visu==0){
			output_visualisation_history();
			output_visualisation_solution();
		}
	}



	// Creation of a checkpoint with the bare minimum data to restart the simulation (i.e nodes information,
	// and quadrature point information)
	template <int dim>
	void FEProblem<dim>::checkpoint () const
	{
		char filename[1024];

		// Copy of the solution vector at the end of the presently converged time-step.
		MPI_Barrier(FE_communicator);
		if (this_FE_process==0)
		{
			// Write solution vector to binary for simulation restart
			const std::string solution_filename = (macrostatelocres + "/lcts.solution.bin");
			std::ofstream ofile(solution_filename);
			displacement.block_write(ofile);
			ofile.close();

			const std::string solution_filename_veloc = (macrostatelocres + "/lcts.velocity.bin");
			std::ofstream ofile_veloc(solution_filename_veloc);
			velocity.block_write(ofile_veloc);
			ofile_veloc.close();
		}
		MPI_Barrier(FE_communicator);

		// Output of the last converged timestep quadrature local history per processor
		sprintf(filename, "%s/lcts.pr_%d.lhistory.bin", macrostatelocres.c_str(), this_FE_process);
		std::ofstream  lhprocoutbin(filename, std::ios_base::binary);

		// Output of complete local history in a single file per processor
		for (typename DoFHandler<dim>::active_cell_iterator
				cell = dof_handler.begin_active();
				cell != dof_handler.end(); ++cell)
			if (cell->is_locally_owned())
			{
				char cell_id[1024]; sprintf(cell_id, "%d", cell->active_cell_index());

				PointHistory<dim> *local_qp_hist
				= reinterpret_cast<PointHistory<dim> *>(cell->user_pointer());

				// Save strain, updstrain, stress history in one file per proc
				for (unsigned int q=0; q<quadrature_formula.size(); ++q)
				{
					lhprocoutbin << present_time
							<< "," << cell->active_cell_index()
							<< "," << q
							<< "," << local_qp_hist[q].mat.c_str();
					for(unsigned int k=0;k<dim;k++)
						for(unsigned int l=k;l<dim;l++){
							lhprocoutbin << "," << std::setprecision(16) << local_qp_hist[q].upd_strain[k][l];
						}
					for(unsigned int k=0;k<dim;k++)
						for(unsigned int l=k;l<dim;l++){
							lhprocoutbin << "," << std::setprecision(16) << local_qp_hist[q].new_stress[k][l];
						}
					lhprocoutbin << std::endl;
				}
			}
		lhprocoutbin.close();
		MPI_Barrier(FE_communicator);
	}



	template <int dim>
	void FEProblem<dim>::init (int sstp, double tlength,
							   std::string mslocin, std::string mslocout,
							   std::string mslocres, std::string mlogloc,
							   int fchpt, int fovis, int folhis, int folbcf,
							   std::vector<std::string> mdt,
							   std::string twodmfile, double extrudel, int extrudep,
						       boost::property_tree::ptree inconfig){

		input_config = inconfig;		
 
		// Setting up checkpoint and output frequencies
		freq_checkpoint = fchpt;
		freq_output_visu = fovis;
		freq_output_lhist = folhis;
		freq_output_lbcforce = folbcf;


		// Setting up starting timestep number and timestep length
		start_timestep = sstp;
		fe_timestep_length = tlength;

		// Setting up directories location
		macrostatelocin = mslocin;
		macrostatelocout = mslocout;
		macrostatelocres = mslocres;
		macrologloc = mlogloc;

		// Setting up mesh
		twod_mesh_file = twodmfile;
		extrude_length = extrudel;
		extrude_points = extrudep;
		// Setting materials name list
		mdtype = mdt;

		// Setting up common ground direction for rotation from microstructure given orientation

		dcout << " Initiation of the Mesh...       " << std::endl;
		make_grid ();

		dcout << " Initiation of the global vectors and tensor...       " << std::endl;
		setup_system ();

		dcout << " Defining the boundary conditions...			" << std::endl;
		problem_type->define_boundary_conditions(dof_handler);

		dcout << " Initiation of the local tensors...       " << std::endl;
		setup_quadrature_point_history ();
		MPI_Barrier(FE_communicator);

		dcout << " Loading previous simulation data...       " << std::endl;
		restart ();
	}



	template <int dim>
	void FEProblem<dim>::beginstep(int tstp, double ptime){
		timestep = tstp;
		present_time = ptime;

		incremental_velocity = 0;
		incremental_displacement = 0;

		// Setting boudary conditions for current timestep
		set_boundary_values();
	}



	template <int dim>
	void FEProblem<dim>::solve (int nstp, ScaleBridgingData &scale_bridging_data){

		newtonstep = nstp;

		double previous_res;

		dcout << "    Initial assembling FE system..." << std::flush;
		if(timestep==start_timestep) previous_res = assemble_system (true);
		else previous_res = assemble_system (false);

		dcout << "    Initial residual: "
				<< previous_res
				<< std::endl;

		dcout << "    Beginning of timestep: " << timestep << " - newton step: " << newtonstep << std::flush;
		dcout << "    Solving FE system..." << std::flush;

		// Solving for the update of the increment of velocity
		solve_linear_problem_CG();

		// Updating incremental variables
		update_incremental_variables();

		MPI_Barrier(FE_communicator);
		dcout << "    Updating quadrature point data..." << std::endl;

		update_strain_quadrature_point_history(newton_update_displacement);


		MPI_Barrier(FE_communicator);
		write_qp_update_list(scale_bridging_data);

	}



	template <int dim>
	bool FEProblem<dim>::check (ScaleBridgingData scale_bridging_data){
		double previous_res;

		update_stress_quadrature_point_history (scale_bridging_data);

		dcout << "    Re-assembling FE system..." << std::flush;
		previous_res = assemble_system (false);
		MPI_Barrier(FE_communicator);

		dcout << "    Residual: "
				<< previous_res
				<< std::endl;

        // continue_newton is set to false to render the integration explicit, the check of convergence on the residual is disabled
		bool continue_newton = false;
		//if (previous_res>1e-02 and newtonstep < 5) continue_newton = true;

		return continue_newton;

	}



	template <int dim>
	void FEProblem<dim>::endstep (){

		// Updating the total displacement and velocity vectors
		velocity+=incremental_velocity;
		displacement+=incremental_displacement;
		//old_displacement=displacement;

		// Outputs
		output_results ();

		// Saving files for restart
		if(timestep%freq_checkpoint==0){
			//write files here
			//char timeid[1024];
			checkpoint();
       }

        dcout << std::endl;
    }
}
#endif    
