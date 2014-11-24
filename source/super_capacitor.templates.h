#include <cache/super_capacitor.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/grid/grid_in.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/vector_tools.h>
#include <string>
#include <iostream>
#include <iomanip>
#include <fstream>

namespace cache {

template <int dim>                         
SuperCapacitorProblem<dim>::               
SuperCapacitorProblem(std::shared_ptr<boost::property_tree::ptree const> database)
    : dof_handler(triangulation)
    , symmetric_correction(true)
{ 
    std::cout<<"initialize...\n";
    this->build_triangulation(database);
    this->set_cell_material_ids_and_face_boundary_ids(database);
    this->initialize_system(database);
}

template <int dim>
void
SuperCapacitorProblem<dim>::
run(std::shared_ptr<boost::property_tree::ptree const> input_params,
    std::shared_ptr<boost::property_tree::ptree>       output_params)
{                                               
    std::cout<<"run...\n";                      
    this->reset(input_params);

    unsigned int const thermal_block         = 1;
    unsigned int const electrochemical_block = 0;
    dealii::Vector<double> & thermal_solution         = this->solution.block(thermal_block        );
    dealii::Vector<double> & electrochemical_solution = this->solution.block(electrochemical_block);

{   // find initial solution for electrochemical
    electrochemical_solution = 0.0;
    double const dummy_time_step = 1.0;
    this->electrochemical_setup_system(dummy_time_step, cache::Initialize);
    unsigned int step = 0;
    double solution_norm;
    double old_solution_norm = 0.0;
    while (true) {
        ++step;
        this->electrochemical_evolve_one_time_step(dummy_time_step);
        solution_norm = electrochemical_solution.l2_norm();
        if (std::abs(solution_norm - old_solution_norm) < 1.0e-8) {
            break;
        } // end if
        old_solution_norm = solution_norm;
    } // end while
    std::cout<<step<<" iterations\n";
}

    thermal_solution = 0.0; // TODO: initialize to ambient temperature
    

    double const time_step    = input_params->get<double>("time_step"   );
    double const initial_time = input_params->get<double>("initial_time");
    double const final_time   = input_params->get<double>("final_time"  );

    double time = initial_time;
    unsigned int step = 0;
    double data[N_DATA];

    this->thermal_setup_system(time_step);
    this->electrochemical_setup_system(time_step, cache::GalvanostaticCharge);
    while (time <= final_time) {
        ++step;
        time += time_step;
        this->electrochemical_evolve_one_time_step(time_step);
        double temperature = thermal_solution.linfty_norm();
        this->process_solution(data);
        std::cout<<std::setprecision(5)<<"t="<<time<<"  "
            <<"U="<<data[VOLTAGE]<<"  "
            <<"I="<<data[CURRENT]<<"  "
            <<"Q="<<data[JOULE_HEATING]<<"\n";
        if (data[VOLTAGE] >= 2.2) {
            break;
        } //
        // TODO: ...
    } // end while

    this->electrochemical_setup_system(time_step, cache::PotentiostaticDischarge);
    while (time <= final_time) {
        ++step;
        time += time_step;
        this->electrochemical_evolve_one_time_step(time_step);
        this->process_solution(data);
        std::cout<<std::setprecision(5)<<"t="<<time<<"  "
            <<"U="<<data[VOLTAGE]<<"  "
            <<"I="<<data[CURRENT]<<"  "
            <<"Q="<<data[JOULE_HEATING]<<"\n";
    } // end while

    std::vector<double> max_temperature(10, 1.0);          
    std::vector<double> heat_production(10, 2.0);          
    std::vector<double> voltage(10, 3.0);
    std::vector<double> current(10, 4.0);
    std::vector<double> t(10, 5.0);
    std::vector<std::string> capacitor_state(10, "charging");
    std::vector<int> cycle(10, 7);
    
    output_params->put("max_temperature", to_string(max_temperature));
    output_params->put("heat_production", to_string(heat_production));
    output_params->put("voltage",         to_string(voltage));
    output_params->put("current",         to_string(current));
    output_params->put("time",            to_string(t));
    output_params->put("capacitor_state", to_string(capacitor_state));
    output_params->put("cycle",           to_string(cycle));
}                                               

template <int dim>
void 
SuperCapacitorProblem<dim>::
build_triangulation(std::shared_ptr<boost::property_tree::ptree const> database) 
{
    dealii::GridIn<dim> mesh_reader;
    std::fstream fin;
    std::string const filename = "mesh_" + std::to_string(dim) + "d.ucd";
    fin.open(filename.c_str(), std::fstream::in);
    mesh_reader.attach_triangulation(this->triangulation);
    mesh_reader.read_ucd(fin);
    fin.close();
    std::cout<<"cells="<<this->triangulation.n_active_cells()<<"  "
        <<"faces="<<this->triangulation.n_active_faces()<<"  "
        <<"vertices="<<this->triangulation.n_used_vertices()<<"\n";
}

template <int dim>
void SuperCapacitorProblem<dim>::set_cell_material_ids_and_face_boundary_ids(std::shared_ptr<boost::property_tree::ptree const> database) {
    double const electrode_width = 50.0e-6;
    double const separator_width = 25.0e-6;
    double const collector_width = 5.0e-6;
    double const x_min = -collector_width;
    double const x_max = 2.0 * electrode_width + separator_width + collector_width;
    double const y_min = -5.0e-6;
    double const y_max = 30.0e-6;
    double const y_top = 25.0e-6;
    double const y_bottom = 0.0e-6;
  
    dealii::types::material_id const separator_material_id         = database->get<dealii::types::material_id>("separator_material_id"        );
    dealii::types::material_id const anode_electrode_material_id   = database->get<dealii::types::material_id>("anode_electrode_material_id"  );
    dealii::types::material_id const anode_collector_material_id   = database->get<dealii::types::material_id>("anode_collector_material_id"  );
    dealii::types::material_id const cathode_electrode_material_id = database->get<dealii::types::material_id>("cathode_electrode_material_id");
    dealii::types::material_id const cathode_collector_material_id = database->get<dealii::types::material_id>("cathode_collector_material_id");

    dealii::types::boundary_id const cathode_boundary_id = database->get<dealii::types::boundary_id>("cathode_boundary_id");
    dealii::types::boundary_id const anode_boundary_id   = database->get<dealii::types::boundary_id>("anode_boundary_id"  );
    dealii::types::boundary_id const upper_boundary_id   = database->get<dealii::types::boundary_id>("upper_boundary_id"  );
    dealii::types::boundary_id const lower_boundary_id   = database->get<dealii::types::boundary_id>("lower_boundary_id"  );
    dealii::types::boundary_id const other_boundary_id   = database->get<dealii::types::boundary_id>("other_boundary_id"  );

    typename dealii::Triangulation<dim>::active_cell_iterator cell = this->triangulation.begin_active();
    typename dealii::Triangulation<dim>::active_cell_iterator end_cell = this->triangulation.end();
    for ( ; cell != end_cell; ++cell) {
        if ((cell->center()[0] > x_min)
            && (cell->center()[0] < x_min + collector_width)) {
            cell->set_material_id(anode_collector_material_id);
        } else if ((cell->center()[0] > x_min + collector_width)
            && (cell->center()[0] < x_min + collector_width + electrode_width)) {
            cell->set_material_id(anode_electrode_material_id);
        } else if ((cell->center()[0] > x_min + collector_width + electrode_width)
            && (cell->center()[0] < x_min + collector_width + electrode_width + separator_width)) {
            cell->set_material_id(separator_material_id);
        } else if ((cell->center()[0] > x_min + collector_width + electrode_width + separator_width)
            && (cell->center()[0] < x_min + collector_width + 2.0*electrode_width + separator_width)) {
            cell->set_material_id(cathode_electrode_material_id);
        } else if ((cell->center()[0] > x_min + collector_width + 2.0*electrode_width + separator_width)
            && (cell->center()[0] < x_max)) {
            cell->set_material_id(cathode_collector_material_id);
        } else {
            std::runtime_error("Error while setting material ids");
        } // end if
        for (std::size_t face = 0; face < dealii::GeometryInfo<dim>::faces_per_cell; ++face) {
            if (cell->face(face)->at_boundary()) {
                if ((std::abs(cell->face(face)->center()[1] - y_max) < 1.0e-10)
                    && (cell->material_id() == anode_collector_material_id)) {
                    cell->face(face)->set_boundary_indicator(anode_boundary_id);
                } else if ((std::abs(cell->face(face)->center()[1] - y_min) < 1.0e-10)
                    && (cell->material_id() == cathode_collector_material_id)) {
                    cell->face(face)->set_boundary_indicator(cathode_boundary_id);
                } else if ((std::abs(cell->face(face)->center()[1] - y_top) < 1.0e-10)
                    && ((cell->material_id() == cathode_electrode_material_id)
                        || (cell->material_id() == anode_electrode_material_id)
                        || (cell->material_id() == separator_material_id))
                    ) {
                    cell->face(face)->set_boundary_indicator(upper_boundary_id);
                } else if ((std::abs(cell->face(face)->center()[1] - y_bottom) < 1.0e-10)
                    && ((cell->material_id() == cathode_electrode_material_id)
                        || (cell->material_id() == anode_electrode_material_id)
                        || (cell->material_id() == separator_material_id))
                    ) {
                    cell->face(face)->set_boundary_indicator(lower_boundary_id);
                } else {
                    cell->face(face)->set_boundary_indicator(other_boundary_id);
                } // end if
            } // end if face at boundary
        } // end for face
    } // end for cell
}



template <int dim>
void 
SuperCapacitorProblem<dim>::
initialize_system(std::shared_ptr<boost::property_tree::ptree const> database) 
{
    // distribute degrees of freedom
    this->fe = std::make_shared<typename dealii::FESystem<dim> >
        (typename dealii::FE_Q<dim>(1), 3); // TODO: read degree p from database
    this->dof_handler.distribute_dofs(*this->fe);
    dealii::DoFRenumbering::component_wise(this->dof_handler);
    unsigned int const n_components = dealii::DoFTools::n_components(this->dof_handler);
    std::vector<dealii::types::global_dof_index> dofs_per_component(n_components);
    dealii::DoFTools::count_dofs_per_component(this->dof_handler, dofs_per_component);

    unsigned int const temperature_component      = database->get<unsigned int>("temperature_component"     );
    unsigned int const solid_potential_component  = database->get<unsigned int>("solid_potential_component" );
    unsigned int const liquid_potential_component = database->get<unsigned int>("liquid_potential_component");
    unsigned int const thermal_block              = database->get<unsigned int>("thermal_block"             );
    unsigned int const electrochemical_block      = database->get<unsigned int>("electrochemical_block"     );
    dealii::types::global_dof_index const n_thermal_dofs         = dofs_per_component[temperature_component];
    dealii::types::global_dof_index const n_electrochemical_dofs = 
        dofs_per_component[solid_potential_component] + dofs_per_component[liquid_potential_component];

    // make sparsity pattern
    unsigned int const max_couplings = this->dof_handler.max_couplings_between_dofs();
    this->sparsity_pattern.reinit(2, 2);
    this->sparsity_pattern.block(electrochemical_block, electrochemical_block).reinit(n_electrochemical_dofs, n_electrochemical_dofs, 2*max_couplings);
    this->sparsity_pattern.block(electrochemical_block, thermal_block        ).reinit(n_electrochemical_dofs, n_thermal_dofs,           max_couplings);
    this->sparsity_pattern.block(thermal_block        , electrochemical_block).reinit(n_thermal_dofs,         n_electrochemical_dofs, 2*max_couplings);
    this->sparsity_pattern.block(thermal_block        , thermal_block        ).reinit(n_thermal_dofs,         n_thermal_dofs,           max_couplings);
    this->sparsity_pattern.collect_sizes();

    dealii::DoFTools::make_sparsity_pattern(this->dof_handler, this->sparsity_pattern, this->constraint_matrix);
    this->sparsity_pattern.compress();

    // initialize matrices and vectors
    this->system_matrix.reinit(this->sparsity_pattern);
    this->system_rhs.reinit(2);
    this->system_rhs.block(electrochemical_block).reinit(n_electrochemical_dofs);
    this->system_rhs.block(thermal_block        ).reinit(n_thermal_dofs        );
    this->system_rhs.collect_sizes();

    this->solution.reinit(this->system_rhs);

    // TODO: add const ref thermal_solution and elctrochemical_solution for
    // readibility
    this->thermal_load_vector.reinit(this->system_rhs.block(thermal_block));


    std::cout
        <<"total degrees of freedom : "<<this->dof_handler.n_dofs()<<"\n"
        <<"    electrochemical : "
        <<"solid_potential "<<dofs_per_component[solid_potential_component]
        <<" + liquid_potential "<<dofs_per_component[liquid_potential_component]
        <<"\n"
        <<"    thermal         : "
        <<"temperature "<<dofs_per_component[temperature_component]
        <<"\n";
}

template <int dim>
void 
SuperCapacitorProblem<dim>::
reset(std::shared_ptr<boost::property_tree::ptree const> database) 
{
    std::shared_ptr<boost::property_tree::ptree> material_properties_database = 
        std::make_shared<boost::property_tree::ptree>(database->get_child("material_properties"));
    std::shared_ptr<SuperCapacitorMPValues<dim> > mp_values = std::shared_ptr<SuperCapacitorMPValues<dim> >
        (new SuperCapacitorMPValues<dim>(SuperCapacitorMPValuesParameters<dim>(material_properties_database)));

    std::shared_ptr<boost::property_tree::ptree> boundary_values_database = 
        std::make_shared<boost::property_tree::ptree>(database->get_child("boundary_values"));
    std::shared_ptr<SuperCapacitorBoundaryValues<dim> > boundary_values = std::shared_ptr<SuperCapacitorBoundaryValues<dim> >
        (new SuperCapacitorBoundaryValues<dim>(SuperCapacitorBoundaryValuesParameters<dim>(boundary_values_database)));

    // initialize operators
    this->electrochemical_operator_params = std::shared_ptr<cache::ElectrochemicalOperatorParameters<dim> >
        (new cache::ElectrochemicalOperatorParameters<dim>(database));
    this->electrochemical_operator_params->dof_handler       = &(this->dof_handler);
    this->electrochemical_operator_params->constraint_matrix = &(this->constraint_matrix);
    this->electrochemical_operator_params->sparsity_pattern  = &(this->sparsity_pattern.block(0, 0));
    this->electrochemical_operator_params->some_vector       = &(this->solution.block(0));
    this->electrochemical_operator_params->mp_values         = std::dynamic_pointer_cast<MPValues<dim>       const>(mp_values);
    this->electrochemical_operator_params->boundary_values   = std::dynamic_pointer_cast<BoundaryValues<dim> const>(boundary_values);
    this->electrochemical_operator = std::shared_ptr<cache::ElectrochemicalOperator<dim> >
        (new cache::ElectrochemicalOperator<dim>(this->electrochemical_operator_params));

    // set null space
    this->electrochemical_operator->set_null_space(database->get<unsigned int>("solid_potential_component" ), database->get<dealii::types::material_id>("material_properties.separator_material_id"        ));
    this->electrochemical_operator->set_null_space(database->get<unsigned int>("liquid_potential_component"), database->get<dealii::types::material_id>("material_properties.anode_collector_material_id"  ));
    this->electrochemical_operator->set_null_space(database->get<unsigned int>("liquid_potential_component"), database->get<dealii::types::material_id>("material_properties.cathode_collector_material_id"));
    std::vector<dealii::types::global_dof_index> const & null_space = this->electrochemical_operator->get_null_space();
    std::cout<<"null space size : "<<null_space.size()<<"\n";

    this->thermal_operator_params = std::shared_ptr<cache::ThermalOperatorParameters<dim> >
        (new cache::ThermalOperatorParameters<dim>(database));
    this->thermal_operator_params->dof_handler       = &(this->dof_handler);
    this->thermal_operator_params->constraint_matrix = &(this->constraint_matrix);
    this->thermal_operator_params->sparsity_pattern  = &(this->sparsity_pattern.block(1, 1));
    this->thermal_operator_params->some_vector       = &(this->solution.block(1));
    this->thermal_operator_params->mp_values         = std::dynamic_pointer_cast<MPValues<dim>       const>(mp_values);
    this->thermal_operator_params->boundary_values   = std::dynamic_pointer_cast<BoundaryValues<dim> const>(boundary_values);
    this->thermal_operator = std::shared_ptr<cache::ThermalOperator<dim> >
        (new ThermalOperator<dim>(this->thermal_operator_params));


}

template <int dim>
void
SuperCapacitorProblem<dim>::
thermal_setup_system(double time_step)
{
    this->thermal_operator->reset(this->thermal_operator_params);
    // TODO: won't work because of dof shift
    AssertThrow((this->thermal_operator)->get_boundary_values().size() == 0,
        dealii::StandardExceptions::ExcMessage("Under construction"));

    unsigned int const thermal_block = 1; // TODO: ...
    dealii::SparseMatrix<double> & thermal_matrix     = this->system_matrix.block(thermal_block, thermal_block);
    dealii::Vector<double> &       thermal_solution   = this->solution     .block(thermal_block);
    dealii::Vector<double> &       thermal_rhs_vector = this->system_rhs   .block(thermal_block);

    this->system_matrix.block(1, 1).copy_from(this->thermal_operator->get_mass_matrix());
    this->system_matrix.block(1, 1).add(time_step, this->thermal_operator->get_stiffness_matrix());

    dealii::MatrixTools::apply_boundary_values(this->thermal_operator->get_boundary_values(), this->system_matrix.block(1, 1), this->solution.block(1), this->system_rhs.block(1), false);

    this->inverse_thermal_system_matrix.initialize(this->system_matrix.block(1, 1));

}

template <int dim>
void SuperCapacitorProblem<dim>::
thermal_evolve_one_time_step(double const time_step) 
{   
    AssertThrow((this->thermal_operator)->get_boundary_values().size() == 0,
        dealii::StandardExceptions::ExcMessage("Under construction"));
    
    this->thermal_operator->get_mass_matrix().vmult(this->system_rhs.block(1), this->solution.block(1));
    this->thermal_load_vector = this->thermal_operator->get_load_vector();
    this->electrochemical_operator->compute_heat_source(this->solution, this->thermal_load_vector);
    this->system_rhs.block(1).add(time_step, thermal_load_vector);
    
    this->inverse_thermal_system_matrix.vmult(this->solution.block(1), this->system_rhs.block(1));
}

template <int dim>
void SuperCapacitorProblem<dim>::
electrochemical_evolve_one_time_step(double const time_step)
{ 
    std::map<dealii::types::global_dof_index, double>::const_iterator it,
        begin_it = this->electrochemical_operator->get_boundary_values().cend(),
        end_it = this->electrochemical_operator->get_boundary_values().cend();
    if (this->symmetric_correction) {
        for (it = begin_it ; it != end_it; ++it) {
            this->solution.block(0)[it->first] = 0.0;
        } // end for
    } // end if symmetric correction
        (this->electrochemical_operator)->get_mass_matrix().vmult(this->system_rhs.block(0), this->solution.block(0));
    for (it = begin_it ; it != end_it; ++it) {
        this->system_rhs.block(0)[it->first] = 0.0;
        this->solution.block(0)[it->first] = it->second;
    } // end for
    this->system_rhs.block(0).add(time_step, this->electrochemical_operator->get_load_vector());

    begin_it = this->rhs_set.cbegin();
    end_it = this->rhs_set.cend();
    for (it = begin_it ; it != end_it; ++it) {
        this->system_rhs.block(0)[it->first] = it->second;
    } // end for
    if (this->symmetric_correction) {
        begin_it = this->rhs_add.cbegin();
        end_it = this->rhs_add.cend();
        for (it = begin_it ; it != end_it; ++it) {
            this->system_rhs.block(0)[it->first] += it->second;
        } // end for
    } // end if symmetric correction

    this->inverse_electrochemical_system_matrix.vmult(this->solution.block(0), this->system_rhs.block(0));
}

template <int dim>
void
SuperCapacitorProblem<dim>::
electrochemical_setup_system(double const time_step, cache::CapacitorState const capacitor_state) 
{   
    this->electrochemical_operator_params->capacitor_state = capacitor_state;
    this->electrochemical_operator->reset(this->electrochemical_operator_params);
        
    this->system_matrix.block(0, 0).copy_from(this->electrochemical_operator->get_mass_matrix());
    this->system_matrix.block(0, 0).add(time_step, (this->electrochemical_operator)->get_stiffness_matrix());
    
    std::vector<dealii::types::global_dof_index> const & null_space = this->electrochemical_operator->get_null_space();
    this->system_matrix.block(0, 0).set(null_space, dealii::FullMatrix<double>(dealii::IdentityMatrix(null_space.size())));
    
    this->system_rhs.block(0) = 0.0;
    dealii::MatrixTools::apply_boundary_values(this->electrochemical_operator->get_boundary_values(), this->system_matrix.block(0, 0), this->solution.block(0), this->system_rhs.block(0), this->symmetric_correction);

    this->inverse_electrochemical_system_matrix.initialize(this->system_matrix.block(0, 0));
        
    this->rhs_set.clear();
    this->rhs_add.clear();
    std::map<dealii::types::global_dof_index, double>::const_iterator it = this->electrochemical_operator->get_boundary_values().cbegin();
    std::map<dealii::types::global_dof_index, double>::const_iterator end_it = this->electrochemical_operator->get_boundary_values().cend();
    for ( ; it != end_it; ++it) {
        this->rhs_set[it->first] = this->system_rhs.block(0)[it->first];
        this->system_rhs.block(0)[it->first] = 0.0;
    } // end for
    AssertThrow(rhs_set.size() == this->electrochemical_operator->get_boundary_values().size(),
        dealii::StandardExceptions::ExcDimensionMismatch(rhs_set.size(), this->electrochemical_operator->get_boundary_values().size()));
    if ((this->symmetric_correction)
        && (this->system_rhs.block(0).l2_norm() != 0.0)) {
        dealii::types::global_dof_index n = this->system_rhs.block(0).size();
        for (dealii::types::global_dof_index i = 0; i < n; ++i) {
            if (this->system_rhs.block(0)[i] != 0.0) {
                this->rhs_add[i] = this->system_rhs.block(0)[i];
            } // end if entry is non zero
        } // end for all entries
    } // end if symetric correction
    std::cout<<"rhs set size is "<<rhs_set.size()<<"\n"
        "rhs add size is "<<rhs_add.size()<<"\n";
}

template <int dim>
void
SuperCapacitorProblem<dim>::
process_solution(double * data) 
{   
    std::fill(data, data+N_DATA, 0.0);
    dealii::types::boundary_id const cathode_boundary_id = 1; //database->get<dealii::types::boundary_id>("cathode_boundary_id");
    std::shared_ptr<MPValues<dim> const> mp_values = electrochemical_operator_params->mp_values;
    unsigned int const temperature_component      = 2; //database->get<unsigned int>("temperature_component"     );
    unsigned int const solid_potential_component  = 0; //database->get<unsigned int>("solid_potential_component" );
    unsigned int const liquid_potential_component = 1; //database->get<unsigned int>("liquid_potential_component");
    dealii::FEValuesExtractors::Scalar const solid_potential (solid_potential_component );
    dealii::FEValuesExtractors::Scalar const liquid_potential(liquid_potential_component);
    dealii::FEValuesExtractors::Scalar const temperature     (temperature_component     );

    dealii::Triangulation<dim> const & tria_ = this->dof_handler.get_tria();
    dealii::Vector<double> joule_heating(tria_.n_active_cells());

    dealii::FiniteElement<dim> const & fe_ = this->dof_handler.get_fe(); // TODO: don't want to use directly fe because we might create postprocessor that will only know about dof_handler
    dealii::QGauss<dim>   quadrature_rule     (fe_.degree+1);
    dealii::QGauss<dim-1> face_quadrature_rule(fe_.degree+1);
    dealii::FEValues<dim>     fe_values     (fe_, quadrature_rule,      dealii::update_values | dealii::update_gradients | dealii::update_JxW_values);
    dealii::FEFaceValues<dim> fe_face_values(fe_, face_quadrature_rule, dealii::update_values | dealii::update_gradients | dealii::update_JxW_values | dealii::update_normal_vectors);
    unsigned int const dofs_per_cell   = fe_.dofs_per_cell;
    unsigned int const n_q_points      = quadrature_rule     .size();
    unsigned int const n_face_q_points = face_quadrature_rule.size();
    std::vector<double>                  solid_electrical_conductivity_values (n_q_points);
    std::vector<double>                  liquid_electrical_conductivity_values(n_q_points);
    std::vector<dealii::Tensor<1, dim> > solid_potential_gradients            (n_q_points);
    std::vector<dealii::Tensor<1, dim> > liquid_potential_gradients           (n_q_points);

    std::vector<double>                  face_solid_electrical_conductivity_values(n_face_q_points);
    std::vector<double>                  face_solid_potential_values              (n_face_q_points);
    std::vector<dealii::Tensor<1, dim> > face_solid_potential_gradients           (n_face_q_points);
    std::vector<dealii::Point<dim> >     normal_vectors                           (n_face_q_points);
    typename dealii::DoFHandler<dim>::active_cell_iterator
        cell = this->dof_handler.begin_active(),
        end_cell = this->dof_handler.end();
    for ( ; cell != end_cell; ++cell) {
        fe_values.reinit(cell);
        mp_values->get_values("solid_electrical_conductivity",  cell, solid_electrical_conductivity_values );
        mp_values->get_values("liquid_electrical_conductivity", cell, liquid_electrical_conductivity_values);
        fe_values[solid_potential ].get_function_gradients(this->solution, solid_potential_gradients );
        fe_values[liquid_potential].get_function_gradients(this->solution, liquid_potential_gradients);
        for (unsigned int q_point = 0; q_point < n_q_points; ++q_point) {
            data[JOULE_HEATING] += 
              ( solid_electrical_conductivity_values [q_point] * solid_potential_gradients [q_point] * solid_potential_gradients [q_point]
              + liquid_electrical_conductivity_values[q_point] * liquid_potential_gradients[q_point] * liquid_potential_gradients[q_point]
              ) * fe_values.JxW(q_point);
            data[VOLUME] += fe_values.JxW(q_point);
        } // end for quadrature point
        if (cell->at_boundary()) {
            for (unsigned int face = 0; face < dealii::GeometryInfo<dim>::faces_per_cell; ++face) {
                if (cell->face(face)->at_boundary()) {
if (cell->face(face)->boundary_indicator() == cathode_boundary_id) {
                    fe_face_values.reinit(cell, face);
                    mp_values->get_values("solid_electrical_conductivity", cell, face_solid_electrical_conductivity_values); // TODO: should take face as an argument...
                    fe_face_values[solid_potential].get_function_gradients(this->solution, face_solid_potential_gradients);
                    fe_face_values[solid_potential].get_function_values   (this->solution, face_solid_potential_values   );
                    normal_vectors = fe_face_values.get_normal_vectors();
                    for (unsigned int face_q_point = 0; face_q_point < n_face_q_points; ++face_q_point) {
                        data[CURRENT] += 
                          ( face_solid_electrical_conductivity_values [face_q_point] * face_solid_potential_gradients[face_q_point] 
                          * normal_vectors[face_q_point] 
                          ) * fe_face_values.JxW(face_q_point); 
                        data[VOLTAGE] += face_solid_potential_values[face_q_point] * fe_face_values.JxW(face_q_point);
                        data[SURFACE_AREA] += fe_face_values.JxW(face_q_point);
                    } // end for face quadrature point
} // end if cathode
                } // end if face at boundary
            } // end for face
        } // end if cell at boundary
    } // end for cell
    data[VOLTAGE] /= data[SURFACE_AREA];
//    std::cout
//        <<"I="<<data[CURRENT]<<"  "
//        <<"U="<<data[VOLTAGE]<<"  "
//        <<"Q="<<data[JOULE_HEATING]<<"  "
//        <<"A="<<data[SURFACE_AREA]<<"  "
//        <<"V="<<data[VOLUME]<<"\n";
}          

} // end namespace cache
