/*
 * @BEGIN LICENSE
 *
 * mcpdft by Psi4 Developer, a plugin to:
 *
 * Psi4: an open-source quantum chemistry software package
 *
 * Copyright (c) 2007-2016 The Psi4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * @END LICENSE
 */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <vector>
#include <utility>
#include <tuple>
#include <fstream>
#include <iostream>
#include <iomanip>

#include "psi4/libpsi4util/libpsi4util.h"

#include "psi4/libqt/qt.h"

// jk object
#include "psi4/libfock/jk.h"

// for dft
#include "psi4/libfock/v.h"
#include "psi4/libfunctional/superfunctional.h"

// for grid
#include "psi4/libfock/points.h"
#include "psi4/libfock/cubature.h"

#include "psi4/psi4-dec.h"
#include "psi4/liboptions/liboptions.h"
#include "psi4/libpsio/psio.hpp"

#include "psi4/libmints/wavefunction.h"
#include "psi4/libmints/mintshelper.h"
#include "psi4/libmints/matrix.h"
#include "psi4/libmints/vector.h"
#include "psi4/libmints/basisset.h"
#include "psi4/libmints/molecule.h"
#include "psi4/lib3index/dftensor.h"
#include "psi4/libqt/qt.h"

// for potential object
#include "psi4/libfock/v.h"
#include "psi4/libfunctional/superfunctional.h"
#include "psi4/libscf_solver/hf.h"

// mcpdft 
#include "mcpdft_solver.h"

// blas
#include "blas.h"
#include "blas_mangle.h"

using namespace psi;
using namespace fnocc;

namespace psi{ namespace mcpdft {

// the MCPDFTSolver class derives from the Wavefunction class and inherits its members
MCPDFTSolver::MCPDFTSolver(std::shared_ptr<Wavefunction> reference_wavefunction,Options & options_):
    Wavefunction(options_){

    reference_wavefunction_ = reference_wavefunction;
    common_init();
}

MCPDFTSolver::~MCPDFTSolver() {

}

// initialize members of the MCPDFTSolver class
void MCPDFTSolver::common_init() {

    shallow_copy(reference_wavefunction_);

    // number of alpha electrons
    nalpha_   = reference_wavefunction_->nalpha();

    // number of beta electrons
    nbeta_    = reference_wavefunction_->nbeta();

    // number of alpha electrons per irrep
    nalphapi_ = reference_wavefunction_->nalphapi();

    // number of beta electrons per irrep
    nbetapi_  = reference_wavefunction_->nbetapi();

    // number of doubly occupied orbitals per irrep
    doccpi_   = reference_wavefunction_->doccpi();

    // number of singly occupied orbitals per irrep
    soccpi_   = reference_wavefunction_->soccpi();

    // number of frozen core orbitals per irrep
    frzcpi_   = reference_wavefunction_->frzcpi();

    // number of frozen virtual orbitals per irrep
    frzvpi_   = reference_wavefunction_->frzvpi();

    // number of molecular orbials per irrep
    nmopi_    = reference_wavefunction_->nmopi();

    // number of symmetry orbials per irrep
    nsopi_    = reference_wavefunction_->nsopi();

    // number of irreducible representations
    nirrep_   = reference_wavefunction_->nirrep();

    // make sure we are running in c1 symmetry
    // if ( nirrep_ > 1 ) {
    //     throw PsiException("plugin mcpdft only works with symmetry c1",__FILE__,__LINE__);
    // }

    // total number of symmetry orbitals
    nso_      = reference_wavefunction_->nso();

    // total number of molecular orbitals
    nmo_      = reference_wavefunction_->nmo();

    // grab the molecule from the reference wave function
    molecule_ = reference_wavefunction_->molecule();

    // SO/MO transformation matrices
    Ca_ = std::shared_ptr<Matrix>(reference_wavefunction_->Ca());
    Cb_ = std::shared_ptr<Matrix>(reference_wavefunction_->Cb());

    // overlap integrals
    S_  = std::shared_ptr<Matrix>(reference_wavefunction_->S());

    // SO-basis Fock matrices
    Fa_ = std::shared_ptr<Matrix>(reference_wavefunction_->Fa());
    Fb_ = std::shared_ptr<Matrix>(reference_wavefunction_->Fb());

    // SO-basis density matrices
    Da_ = std::shared_ptr<Matrix>(reference_wavefunction_->Da());
    Db_ = std::shared_ptr<Matrix>(reference_wavefunction_->Db());

    // orbital energies
    epsilon_a_= std::shared_ptr<Vector>(new Vector(nirrep_, nmopi_));
    epsilon_a_->copy(reference_wavefunction_->epsilon_a().get());
    epsilon_b_= std::shared_ptr<Vector>(new Vector(nirrep_, nmopi_));
    epsilon_b_->copy(reference_wavefunction_->epsilon_b().get());

    // memory is from process::environment
    memory_ = Process::environment.get_memory();

    // set the wavefunction name
    name_ = "DFT";

    // restricted orbitals, unrestricted rdms
    same_a_b_orbs_ = true;
    same_a_b_dens_ = false;

    // symmetry of orbitals:
    symmetry_ = (int*)malloc(nmo_*sizeof(int));
    memset((void*)symmetry_,'\0',nmo_*sizeof(int));
    int count = 0;
    for (int h = 0; h < nirrep_; h++) {
        for (int i = 0; i < nsopi_[h]; i++) {
            symmetry_[count++] = h;
        }
    }

    // pitzer offset
    pitzer_offset_ = (int*)malloc(nirrep_*sizeof(int));
    count = 0;
    for (int h = 0; h < nirrep_; h++) {
        pitzer_offset_[h] = count;
        count += nsopi_[h];
    }

    // obtain phi(r), del phi(r)

    // evaluate basis function values on a grid:

    outfile->Printf("\n");
    outfile->Printf("    ==> Build Phi and Phi' matrices <==\n");

    scf::HF* scfwfn = (scf::HF*)reference_wavefunction_.get();
    std::shared_ptr<SuperFunctional> functional = scfwfn->functional();
    std::shared_ptr<VBase> potential = scfwfn->V_potential();


    // phi matrix (real-space <- AO basis mapping)
    // since grid is stored in blocks, we need to build a full phi matrix
    // from the individual blocks:

    std::shared_ptr<BasisSet> primary = reference_wavefunction_->get_basisset("ORBITAL");
    int nao = primary->nbf();
    std::shared_ptr<Matrix> Da_ao (new Matrix(nao,nao));
    std::shared_ptr<Matrix> Db_ao (new Matrix(nao,nao));

    std::shared_ptr<PointFunctions> points_func = potential->properties()[0];
    points_func->set_pointers(Da_ao,Db_ao);

    // determine number of grid points
    int nblocks = potential->nblocks();
    phi_points_       = 0;
    int max_functions = 0;
    int max_points    = 0;
    for (int myblock = 0; myblock < nblocks; myblock++) {
        std::shared_ptr<BlockOPoints> block = potential->get_block(myblock);
        points_func->compute_points(block);
        int npoints = block->npoints();
        phi_points_ += npoints;
        const std::vector<int>& function_map = block->functions_local_to_global();
        int nlocal = function_map.size();
        if ( nlocal > max_functions ) max_functions = nlocal;
        if ( npoints > max_points )   max_points    = npoints;
    }

    // what is the derivative level?
    deriv_   = functional->deriv();
    is_gga_  = functional->is_gga();
    is_meta_ = functional->is_meta();
    is_unpolarized_ = functional->is_unpolarized();

    std::shared_ptr<Matrix> super_phi_ao (new Matrix("SUPER PHI (AO)",phi_points_,nso_));

    std::shared_ptr<Matrix> super_phi_x_ao;
    std::shared_ptr<Matrix> super_phi_y_ao;
    std::shared_ptr<Matrix> super_phi_z_ao;

    // std::shared_ptr<Matrix> super_phi_xx_ao;
    // std::shared_ptr<Matrix> super_phi_xy_ao;
    // std::shared_ptr<Matrix> super_phi_xz_ao;
    // std::shared_ptr<Matrix> super_phi_yy_ao;
    // std::shared_ptr<Matrix> super_phi_yz_ao;
    // std::shared_ptr<Matrix> super_phi_zz_ao;

    // std::shared_ptr<Matrix> super_gamma_aa_ao;
    // std::shared_ptr<Matrix> super_gamma_ab_ao;
    // std::shared_ptr<Matrix> super_gamma_bb_ao;

    // std::shared_ptr<Matrix> super_tau_a_ao;
    // std::shared_ptr<Matrix> super_tau_b_ao;

    if ( is_gga_ || is_meta_ ) {

        super_phi_x_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI X (AO)",phi_points_,nso_));
        super_phi_y_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI Y (AO)",phi_points_,nso_));
        super_phi_z_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI Z (AO)",phi_points_,nso_));
        
        // super_phi_xx_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI XX (AO)",phi_points_,nso_));
        // super_phi_xy_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI XY (AO)",phi_points_,nso_));
        // super_phi_xz_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI XZ (AO)",phi_points_,nso_));
        // super_phi_yy_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI YY (AO)",phi_points_,nso_));
        // super_phi_yz_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI YZ (AO)",phi_points_,nso_));
        // super_phi_zz_ao = std::shared_ptr<Matrix>(new Matrix("SUPER PHI ZZ (AO)",phi_points_,nso_));
        // super_gamma_aa_ao = std::shared_ptr<Matrix>(new Matrix("SUPER GAMMA AA (AO)",phi_points_,nso_));
        // super_gamma_ab_ao = std::shared_ptr<Matrix>(new Matrix("SUPER GAMMA AB (AO)",phi_points_,nso_));
        // super_gamma_bb_ao = std::shared_ptr<Matrix>(new Matrix("SUPER GAMMA BB (AO)",phi_points_,nso_));

        // super_tau_a_ao = std::shared_ptr<Matrix>(new Matrix("SUPER TAU A (AO)",phi_points_,nso_));
        // super_tau_b_ao = std::shared_ptr<Matrix>(new Matrix("SUPER TAU B (AO)",phi_points_,nso_));
    }

    grid_x_      = std::shared_ptr<Vector>(new Vector("GRID X",phi_points_));
    grid_y_      = std::shared_ptr<Vector>(new Vector("GRID Y",phi_points_));
    grid_z_      = std::shared_ptr<Vector>(new Vector("GRID Z",phi_points_));
    grid_w_      = std::shared_ptr<Vector>(new Vector("GRID W",phi_points_));

    // build phi matrix and derivative phi matrices (ao basis)
    BuildPhiMatrixAO(potential, points_func, "PHI",super_phi_ao);

    if ( is_gga_ || is_meta_ ) {

        BuildPhiMatrixAO(potential, points_func, "PHI_X",super_phi_x_ao);
        BuildPhiMatrixAO(potential, points_func, "PHI_Y",super_phi_y_ao);
        BuildPhiMatrixAO(potential, points_func, "PHI_Z",super_phi_z_ao);

        // BuildPhiMatrixAO(potential, points_func, "PHI_XX",super_phi_xx_ao);
        // BuildPhiMatrixAO(potential, points_func, "PHI_XY",super_phi_xy_ao);
        // BuildPhiMatrixAO(potential, points_func, "PHI_XZ",super_phi_xz_ao);
        // BuildPhiMatrixAO(potential, points_func, "PHI_YY",super_phi_yy_ao);
        // BuildPhiMatrixAO(potential, points_func, "PHI_YZ",super_phi_yz_ao);
        // BuildPhiMatrixAO(potential, points_func, "PHI_ZZ",super_phi_zz_ao);

        // BuildPhiMatrixAO(potential, points_func, "GAMMA_AA",super_gamma_aa_ao);
        // BuildPhiMatrixAO(potential, points_func, "GAMMA_AB",super_gamma_ab_ao);
        // BuildPhiMatrixAO(potential, points_func, "GAMMA_BB",super_gamma_bb_ao);

        // BuildPhiMatrixAO(potential, points_func, "TAU_A",super_tau_a_ao);
        // BuildPhiMatrixAO(potential, points_func, "TAU_B",super_tau_b_ao);

    }

    // transform the orbital label in phi matrix and derivative phi matrix to the MO basis

    Dimension phi_points_list = Dimension(nirrep_,"phi_points");
    for (int h = 0; h < nirrep_; h++) {
        phi_points_list[h] = phi_points_;
    }

    super_phi_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI",phi_points_list,nsopi_));

    TransformPhiMatrixAOMO(super_phi_ao,super_phi_);

    if ( is_gga_ || is_meta_ ) {

        super_phi_x_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI X",phi_points_list,nsopi_));
        super_phi_y_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI Y",phi_points_list,nsopi_));
        super_phi_z_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI Z",phi_points_list,nsopi_));

        // super_phi_xx_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI XX",phi_points_list,nsopi_));
        // super_phi_xy_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI XY",phi_points_list,nsopi_));
        // super_phi_xz_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI XZ",phi_points_list,nsopi_));
        // super_phi_yy_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI YY",phi_points_list,nsopi_));
        // super_phi_yz_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI YZ",phi_points_list,nsopi_));
        // super_phi_zz_ = std::shared_ptr<Matrix>(new Matrix("SUPER PHI ZZ",phi_points_list,nsopi_));

        // super_gamma_aa_ = std::shared_ptr<Matrix>(new Matrix("SUPER GAMMA AA",phi_points_list,nsopi_));
        // super_gamma_ab_ = std::shared_ptr<Matrix>(new Matrix("SUPER GAMMA AB",phi_points_list,nsopi_));
        // super_gamma_bb_ = std::shared_ptr<Matrix>(new Matrix("SUPER GAMMA BB",phi_points_list,nsopi_));\

        // super_tau_a_ = std::shared_ptr<Matrix>(new Matrix("SUPER TAU A",phi_points_list,nsopi_));
        // super_tau_b_ = std::shared_ptr<Matrix>(new Matrix("SUPER TAU B",phi_points_list,nsopi_));

        TransformPhiMatrixAOMO(super_phi_x_ao,super_phi_x_);
        TransformPhiMatrixAOMO(super_phi_y_ao,super_phi_y_);
        TransformPhiMatrixAOMO(super_phi_z_ao,super_phi_z_);

        // TransformPhiMatrixAOMO(super_phi_xx_ao,super_phi_xx_);
        // TransformPhiMatrixAOMO(super_phi_xy_ao,super_phi_xy_);
        // TransformPhiMatrixAOMO(super_phi_xz_ao,super_phi_xz_);
        // TransformPhiMatrixAOMO(super_phi_yy_ao,super_phi_yy_);
        // TransformPhiMatrixAOMO(super_phi_yz_ao,super_phi_yz_);
        // TransformPhiMatrixAOMO(super_phi_zz_ao,super_phi_zz_);

        // TransformPhiMatrixAOMO(super_gamma_aa_ao,super_gamma_aa_);
        // TransformPhiMatrixAOMO(super_gamma_ab_ao,super_gamma_ab_);
        // TransformPhiMatrixAOMO(super_gamma_bb_ao,super_gamma_bb_);

        // TransformPhiMatrixAOMO(super_tau_a_ao,super_tau_a_);
        // TransformPhiMatrixAOMO(super_tau_b_ao,super_tau_b_);
    }
}

void MCPDFTSolver::BuildPhiMatrixAO(std::shared_ptr<VBase> potential, std::shared_ptr<PointFunctions> points_func,
        std::string phi_type, std::shared_ptr<Matrix> myphi) {

    int nblocks = potential->nblocks();

    phi_points_ = 0;
    for (int myblock = 0; myblock < nblocks; myblock++) {
        std::shared_ptr<BlockOPoints> block = potential->get_block(myblock);
        points_func->compute_points(block);
        int npoints = block->npoints();
        const std::vector<int>& function_map = block->functions_local_to_global();
        int nlocal = function_map.size();

        double ** phi = points_func->basis_value(phi_type)->pointer();

        double * x = block->x();
        double * y = block->y();
        double * z = block->z();
        double * w = block->w();

        // Doing some test to see everything including Pi etc is correct on 
        // Molcas' grid points through comparison.

        // std::ifstream dataIn;
       
        // dataIn.open("H2.grids_test");
        // 
        // if (!dataIn)
        //    std::cout << "Error opening file.\n";
        // else { 
        //      int p = 0;        
        //      while (!dataIn.eof()){
        //    
        //            dataIn >> x[p];
        //            dataIn >> y[p];    
        //            dataIn >> z[p];
        //            p++;
        //      }        
        // }
        // dataIn.close(); 

        // for (int p = 0; p < npoints; p++) {

        //     outfile->Printf("\n     y[");
        //     outfile->Printf("%d",p);
        //     outfile->Printf("] = %20.7lf\n",y[p]);
        // }

        for (int p = 0; p < npoints; p++) {

            grid_x_->pointer()[phi_points_ + p] = x[p];
            grid_y_->pointer()[phi_points_ + p] = y[p];
            grid_z_->pointer()[phi_points_ + p] = z[p];
            grid_w_->pointer()[phi_points_ + p] = w[p];

            for (int nu = 0; nu < nlocal; nu++) {
                int nug = function_map[nu];
                myphi->pointer()[phi_points_ + p][nug] = phi[p][nu];
            }
        }
        phi_points_ += npoints;
    }

}

void MCPDFTSolver::TransformPhiMatrixAOMO(std::shared_ptr<Matrix> phi_in, std::shared_ptr<Matrix> phi_out) {
        
    std::shared_ptr<Matrix> phi_temp (new Matrix(phi_out));

    // grab AO->SO transformation matrix
    std::shared_ptr<Matrix> ao2so = reference_wavefunction_->aotoso();

    for (int h = 0; h < nirrep_; h++) {
        if (nsopi_[h] != 0 )
           F_DGEMM('n','n',nsopi_[h],phi_points_,nso_,1.0,ao2so->pointer(h)[0],nsopi_[h],phi_in->pointer()[0],nso_,0.0,phi_temp->pointer(h)[0],nsopi_[h]);
    }

    for (int h = 0; h < nirrep_; h++) {
        if (nsopi_[h] != 0 )
           F_DGEMM('n','n',nsopi_[h],phi_points_,nsopi_[h],1.0,Ca_->pointer(h)[0],nsopi_[h],phi_temp->pointer(h)[0],nsopi_[h],0.0,phi_out->pointer(h)[0],nsopi_[h]);
    }
}

double MCPDFTSolver::compute_energy() {

    // read 1- and 2-RDM from disk and build rho(r), rho'(r), pi(r), and pi'(r)

    if ( options_.get_str("MCPDFT_REFERENCE_TPDM") == "V2RDM" ) {

        ReadTPDM();
        ReadOPDM();

    }else if ( options_.get_str("MCPDFT_REFERENCE_TPDM") == "CI" ) {

        // read 1-RDM
        ReadCIOPDM(Da_,"opdm_a.txt");
        ReadCIOPDM(Db_,"opdm_b.txt");

        if ( nirrep_ > 1 ) {
            throw PsiException("error, MCPDFT_REFERENCE = CI only works with symmetry c1",__FILE__,__LINE__);
        }

        double * D2ab = (double*)malloc(nmo_*nmo_*nmo_*nmo_*sizeof(double));
        memset((void*)D2ab,'\0',nmo_*nmo_*nmo_*nmo_*sizeof(double));

        // read 2-RDM
        ReadCITPDM(D2ab,"tpdm_ab.txt");

        // build alpha- and beta-spin densities and gradients (already built for MCPDFT_REFERENCE_TPDM = V2RDM)
        outfile->Printf("\n");
        outfile->Printf("    ==> Build Rho <== \n ");

        BuildRho();

        // build on-top pair density (already built for MCPDFT_REFERENCE_TPDM = V2RDM)
        outfile->Printf("\n");
        outfile->Printf("    ==> Build Pi <==\n");

        BuildPi(D2ab);
        free(D2ab);


    }else {
        throw PsiException("invalid MCPDFT_REFERENCE_TPDM type",__FILE__,__LINE__);
    }

    // build R(r) = 4 * Pi(r) / rho(r)
    outfile->Printf("    ==> Build the on-top ratio R <==\n ");
    outfile->Printf("\n");
    Build_R();

    // translate the alpha and beta densities and their corresponding gradients
    if ( options_.get_str("MCPDFT_TRANSLATION_TYPE") == "REGULAR") {

        outfile->Printf("    ==> Regular translation of densities and/or density gradients <==\n ");
        Translate();    

    }else {

        outfile->Printf("    ==> Full translation of densities and/or density gradients <==\n ");
        Fully_Translate();    

    }

    // calculate the on-top energy

    outfile->Printf("    ==> Evaluate the on-top energy contribution <==\n");
    outfile->Printf("\n");

    double mcpdft_xc_energy = 0.0;

    if ( options_.get_str("MCPDFT_FUNCTIONAL") == "SVWN" ) {

        mcpdft_xc_energy = EX_LSDA(tr_rho_a_, tr_rho_b_) 
                         + EC_VWN3_RPA_III(tr_rho_a_, tr_rho_b_);

    }else if ( options_.get_str("MCPDFT_FUNCTIONAL") == "BLYP" ) {
          
        mcpdft_xc_energy = EX_B88_I(tr_rho_a_, tr_rho_b_, tr_sigma_aa_, tr_sigma_bb_)
                         + EC_LYP_I(tr_rho_a_, tr_rho_b_, tr_sigma_aa_, tr_sigma_ab_, tr_sigma_bb_);
    
    }else if ( options_.get_str("MCPDFT_FUNCTIONAL") == "PBE" ) {
 
        mcpdft_xc_energy = EX_PBE_I(tr_rho_a_, tr_rho_b_, tr_sigma_aa_, tr_sigma_bb_) 
                         + EC_PBE_I(tr_rho_a_, tr_rho_b_, tr_sigma_aa_, tr_sigma_ab_, tr_sigma_bb_);

    }else if ( options_.get_str("MCPDFT_FUNCTIONAL") == "BOP" ) {
          
        mcpdft_xc_energy = EX_B88_I(tr_rho_a_, tr_rho_b_, tr_sigma_aa_, tr_sigma_bb_)
                         + EC_B88_OP(tr_rho_a_, tr_rho_b_, tr_sigma_aa_, tr_sigma_bb_);
    }
    
    // evaluate the kinetic, potential, and coulomb energies

    outfile->Printf("    ==> Evaluate kinetic, potential, and coulomb energies <==\n");
    outfile->Printf("\n");

    // one-electron terms:
    std::shared_ptr<MintsHelper> mints(new MintsHelper(reference_wavefunction_));

    SharedMatrix ha (new Matrix(mints->so_potential()));
    ha->add(mints->so_kinetic());
    ha->transform(Ca_);

    SharedMatrix hb (new Matrix(mints->so_potential()));
    hb->add(mints->so_kinetic());
    hb->transform(Cb_);

    //double one_electron_energy = C_DDOT(nmo_*nmo_,D1a,1,ha->pointer()[0],1)
    //                           + C_DDOT(nmo_*nmo_,D1b,1,hb->pointer()[0],1);

    double one_electron_energy = Da_->vector_dot(ha) 
                               + Db_->vector_dot(hb);
    // kinetic-energy 
    SharedMatrix Ka (new Matrix(mints->so_kinetic()));
    Ka->transform(Ca_);
    
    SharedMatrix Kb (new Matrix(mints->so_kinetic()));
    Kb->transform(Cb_);
    
    double kinetic_energy = Da_->vector_dot(Ka) 
                          + Db_->vector_dot(Kb);

    // nuclear-attraction potential energy
    SharedMatrix Va (new Matrix(mints->so_potential()));
    Va->transform(Ca_);
    
    SharedMatrix Vb (new Matrix(mints->so_potential()));
    Vb->transform(Cb_);
    
    double nuclear_attraction_energy = Da_->vector_dot(Va) 
                                     + Db_->vector_dot(Vb);

    // coulomb energy should be computed using J object

    std::vector < std::shared_ptr<Matrix> > J = BuildJ();

    double caa = Da_->vector_dot(J[0]);
    double cab = Da_->vector_dot(J[1]);
    double cba = Db_->vector_dot(J[0]);
    double cbb = Db_->vector_dot(J[1]);

    double coulomb_energy = 0.5 * ( caa + cab + cba + cbb );

    double nuclear_repulsion_energy = molecule_->nuclear_repulsion_energy({0.0,0.0,0.0});

    // print total energy and its components

    outfile->Printf("    ==> Energetics <==\n");
    outfile->Printf("\n");

    // outfile->Printf("        nuclear repulsion energy =    %20.12lf\n",molecule_->nuclear_repulsion_energy());
    outfile->Printf("        nuclear repulsion energy =          %20.12lf\n",nuclear_repulsion_energy);
    outfile->Printf("        nuclear attraction energy =         %20.12lf\n",nuclear_attraction_energy);
    outfile->Printf("        kinetic energy           =          %20.12lf\n",kinetic_energy);
    outfile->Printf("        one-electron energy =               %20.12lf\n",one_electron_energy);
    outfile->Printf("        coulomb energy =                    %20.12lf\n",coulomb_energy);

    if ( options_.get_str("MCPDFT_REFERENCE_TPDM") == "V2RDM") {

        outfile->Printf("        v2RDM-CASSCF energy contribution =  %20.12lf\n",
            nuclear_repulsion_energy + one_electron_energy + coulomb_energy);
  
    }else {

        outfile->Printf("        CASSCF energy contribution =        %20.12lf\n",
            nuclear_repulsion_energy + one_electron_energy + coulomb_energy);

    }

    outfile->Printf("        On-top energy =                     %20.12lf\n",mcpdft_xc_energy);
    outfile->Printf("\n");

    double total_energy = nuclear_repulsion_energy + one_electron_energy + coulomb_energy + mcpdft_xc_energy;
    outfile->Printf("    * MCPDFT total energy =      %20.12lf\n",total_energy);
    outfile->Printf("\n");

    return total_energy;
}

void MCPDFTSolver::BuildPi(double * D2ab) {

    pi_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    double * pi_p = pi_->pointer();

    double ** phi = super_phi_->pointer();

    for (int p = 0; p < phi_points_; p++) {

        double dum = 0.0;

        // pi(r) = D(mu,nu; lambda,sigma) * phi(r,mu) * phi(r,nu) * phi(r,lambda) * phi(r,sigma)
        for (int mu = 0; mu < nmo_; mu++) {
            for (int nu = 0; nu < nmo_; nu++) {
                for (int lambda = 0; lambda < nmo_; lambda++) {
                    for (int sigma = 0; sigma < nmo_; sigma++) {

                        dum += phi[p][mu] * phi[p][lambda] * phi[p][sigma] * phi[p][nu] * D2ab[mu*nmo_*nmo_*nmo_ + nu*nmo_*nmo_ + lambda*nmo_ + sigma];
                    }
                }
            }
        }

        pi_p[p] = dum;
    }

    if ( is_gga_ || is_meta_ ) {

       pi_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       pi_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       pi_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       
       double * pi_xp = pi_x_->pointer();
       double * pi_yp = pi_y_->pointer();
       double * pi_zp = pi_z_->pointer();
       
       double ** phi_x = super_phi_x_->pointer();
       double ** phi_y = super_phi_y_->pointer();
       double ** phi_z = super_phi_z_->pointer();

       for (int p = 0; p < phi_points_; p++) {

           double dum_x = 0.0;
           double dum_y = 0.0;
           double dum_z = 0.0;

           // pi(r) = D(mu,nu; lambda,sigma) * phi(r,mu) * phi(r,nu) * phi(r,lambda) * phi(r,sigma)
           for (int mu = 0; mu < nmo_; mu++) {
               for (int nu = 0; nu < nmo_; nu++) {
                   for (int lambda = 0; lambda < nmo_; lambda++) {
                       for (int sigma = 0; sigma < nmo_; sigma++) {

                           dum_x += ( phi_x[p][mu] * phi[p][lambda] * phi[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi_x[p][lambda] * phi[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi[p][lambda] * phi_x[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi[p][lambda] * phi[p][sigma] * phi_x[p][nu] ) * D2ab[mu*nmo_*nmo_*nmo_ + nu*nmo_*nmo_ + lambda*nmo_ + sigma];

                           dum_y += ( phi_y[p][mu] * phi[p][lambda] * phi[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi_y[p][lambda] * phi[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi[p][lambda] * phi_y[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi[p][lambda] * phi[p][sigma] * phi_y[p][nu] ) * D2ab[mu*nmo_*nmo_*nmo_ + nu*nmo_*nmo_ + lambda*nmo_ + sigma];


                           dum_z += ( phi_z[p][mu] * phi[p][lambda] * phi[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi_z[p][lambda] * phi[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi[p][lambda] * phi_z[p][sigma] * phi[p][nu] +
                                      phi[p][mu] * phi[p][lambda] * phi[p][sigma] * phi_z[p][nu] ) * D2ab[mu*nmo_*nmo_*nmo_ + nu*nmo_*nmo_ + lambda*nmo_ + sigma];
                       }
                   }
               }
           }

           pi_xp[p] = dum_x;
           pi_yp[p] = dum_y;
           pi_zp[p] = dum_z;

           // outfile->Printf("pi_x %15.15lf\n",pi_xp[p]);
           // outfile->Printf("pi_y %15.15lf\n",pi_yp[p]);
           // outfile->Printf("pi_z %15.15lf\n",pi_zp[p]);
           // outfile->Printf("\n    p");
           // outfile->Printf("    x[p]");
           // outfile->Printf("    y[p]");
           // outfile->Printf("    z[p]");
           // outfile->Printf("    pi\n\n");

           // for (int p = 0; p < phi_points_; p++) {

           //     outfile->Printf("    %d %20.5lf %20.5lf %20.5lf %20.15lf\n",p, grid_x_->pointer()[p], grid_y_->pointer()[p], grid_z_->pointer()[p], pi_p[p]);
           // }
       }
    }
}
void MCPDFTSolver::BuildPiFast(tpdm * D2ab, int nab) {

    pi_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    double * pi_p = pi_->pointer();

    for (int p = 0; p < phi_points_; p++) {

        double dum = 0.0;

        // pi(r) = D(mu,nu; lambda,sigma) * phi(r,mu) * phi(r,nu) * phi(r,lambda) * phi(r,sigma)
        for (int n = 0; n < nab; n++) {

            int i = D2ab[n].i;
            int j = D2ab[n].j;
            int k = D2ab[n].k;
            int l = D2ab[n].l;

            int hi = symmetry_[i];
            int hj = symmetry_[j];
            int hk = symmetry_[k];
            int hl = symmetry_[l];

            int ii = i - pitzer_offset_[hi];
            int jj = j - pitzer_offset_[hj];
            int kk = k - pitzer_offset_[hk];
            int ll = l - pitzer_offset_[hl];

            dum += super_phi_->pointer(hi)[p][ii] * 
                   super_phi_->pointer(hj)[p][jj] * 
                   super_phi_->pointer(hk)[p][kk] * 
                   super_phi_->pointer(hl)[p][ll] * D2ab[n].val;

        }

        pi_p[p] = dum;
    }

    if ( is_gga_ || is_meta_ ) {

        pi_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        pi_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        pi_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        
        double * pi_xp = pi_x_->pointer();
        double * pi_yp = pi_y_->pointer();
        double * pi_zp = pi_z_->pointer();
        
        double ** phi_x = super_phi_x_->pointer();
        double ** phi_y = super_phi_y_->pointer();
        double ** phi_z = super_phi_z_->pointer();

        for (int p = 0; p < phi_points_; p++) {

            double dum_x = 0.0;
            double dum_y = 0.0;
            double dum_z = 0.0;

            // pi(r) = D(mu,nu; lambda,sigma) * phi(r,mu) * phi(r,nu) * phi(r,lambda) * phi(r,sigma)
            for (int n = 0; n < nab; n++) {

                int i = D2ab[n].i;
                int j = D2ab[n].j;
                int k = D2ab[n].k;
                int l = D2ab[n].l;
                
                int hi = symmetry_[i];
                int hj = symmetry_[j];
                int hk = symmetry_[k]; 
                int hl = symmetry_[l];
                
                int ii = i - pitzer_offset_[hi];
                int jj = j - pitzer_offset_[hj];
                int kk = k - pitzer_offset_[hk];
                int ll = l - pitzer_offset_[hl];

                dum_x += ( super_phi_x_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_x_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_x_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_x_->pointer(hl)[p][ll] ) * D2ab[n].val;

                dum_y += ( super_phi_y_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_y_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_y_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_y_->pointer(hl)[p][ll] ) * D2ab[n].val;

                dum_z += ( super_phi_z_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_z_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_z_->pointer(hk)[p][kk] * 
                           super_phi_->pointer(hl)[p][ll] +

                           super_phi_->pointer(hi)[p][ii] *
                           super_phi_->pointer(hj)[p][jj] *
                           super_phi_->pointer(hk)[p][kk] * 
                           super_phi_z_->pointer(hl)[p][ll] ) * D2ab[n].val;

            }

            pi_xp[p] = dum_x;
            pi_yp[p] = dum_y;
            pi_zp[p] = dum_z;

            // outfile->Printf("pi_x %15.15lf\n",pi_xp[p]);
            // outfile->Printf("pi_y %15.15lf\n",pi_yp[p]);
            // outfile->Printf("pi_z %15.15lf\n",pi_zp[p]);
            // outfile->Printf("\n    p");
            // outfile->Printf("    x[p]");
            // outfile->Printf("    y[p]");
            // outfile->Printf("    z[p]");
            // outfile->Printf("    pi\n\n");

            // for (int p = 0; p < phi_points_; p++) {

            //     outfile->Printf("    %d %20.5lf %20.5lf %20.5lf %20.15lf\n",p, grid_x_->pointer()[p], grid_y_->pointer()[p], grid_z_->pointer()[p], pi_p[p]);
            // }
        }
    }
}

void MCPDFTSolver::BuildRho() {

    rho_a_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    rho_b_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    rho_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // m_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    // zeta_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // rs_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    double ** phi   = super_phi_->pointer();

    double * rho_ap = rho_a_->pointer();
    double * rho_bp = rho_b_->pointer();
    double * rho_p = rho_->pointer();
    // double * m_p = m_->pointer();
    // 
    // double * zeta_p = zeta_->pointer();
    // double * rs_p = rs_->pointer();
    double temp_tot = 0.0;
    double temp_a = 0.0;
    double temp_b = 0.0;

    double ** dap = Da_->pointer();
    double ** dbp = Db_->pointer();

    for (int p = 0; p < phi_points_; p++) {
        double duma   = 0.0;
        double dumb   = 0.0;
        for (int sigma = 0; sigma < nmo_; sigma++) {
            for (int nu = 0; nu < nmo_; nu++) {
                duma += phi[p][sigma] * phi[p][nu] * dap[sigma][nu];
                dumb += phi[p][sigma] * phi[p][nu] * dbp[sigma][nu];
            }
        }
        rho_ap[p] = duma;
        rho_bp[p] = dumb;
        rho_p[p] = rho_ap[p] + rho_bp[p];    

        temp_tot += rho_p[p] * grid_w_->pointer()[p];
        temp_a += rho_ap[p] * grid_w_->pointer()[p];
        temp_b += rho_bp[p] * grid_w_->pointer()[p];
        // rho_p[p] = rho_ap[p] + rho_bp[p];
        // m_p[p] =  rho_ap[p] - rho_bp[p];

        // zeta_p[p] =  m_p[p]  / rho_p[p];
        // rs_p[p] = pow( 3.0 / ( 4.0 * M_PI * rho_p[p] ) , 1.0/3.0 );
    }
    outfile->Printf("\n");
    outfile->Printf("      Integrated total density = %20.12lf\n",temp_tot);
    outfile->Printf("      Integrated alpha density = %20.12lf\n",temp_a);
    outfile->Printf("      Integrated beta density  = %20.12lf\n",temp_b);
    outfile->Printf("\n");

    if ( is_gga_ || is_meta_ ) {

        sigma_aa_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        sigma_bb_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        sigma_ab_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        rho_a_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        rho_b_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        rho_a_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        rho_b_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        rho_a_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        rho_b_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        // tau_a_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        // tau_b_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        double ** phi_x = super_phi_x_->pointer();
        double ** phi_y = super_phi_y_->pointer();
        double ** phi_z = super_phi_z_->pointer();

        double * sigma_aap = sigma_aa_->pointer();
        double * sigma_bbp = sigma_bb_->pointer();
        double * sigma_abp = sigma_ab_->pointer();
        
        double * rho_a_xp = rho_a_x_->pointer();
        double * rho_b_xp = rho_b_x_->pointer();

        double * rho_a_yp = rho_a_y_->pointer();
        double * rho_b_yp = rho_b_y_->pointer();

        double * rho_a_zp = rho_a_z_->pointer();
        double * rho_b_zp = rho_b_z_->pointer();

        // double * tau_ap = tau_a_->pointer();
        // double * tau_bp = tau_b_->pointer();

        for (int p = 0; p < phi_points_; p++) {
            double duma_x = 0.0;
            double dumb_x = 0.0;
            double duma_y = 0.0;
            double dumb_y = 0.0;
            double duma_z = 0.0;
            double dumb_z = 0.0;
            // double dumta = 0.0;
            // double dumtb = 0.0;

            for (int sigma = 0; sigma < nmo_; sigma++) {
                for (int nu = 0; nu < nmo_; nu++) {
                    duma_x += ( phi_x[p][sigma] * phi[p][nu] + phi[p][sigma] * phi_x[p][nu] ) * dap[sigma][nu];
                    dumb_x += ( phi_x[p][sigma] * phi[p][nu] + phi[p][sigma] * phi_x[p][nu] ) * dbp[sigma][nu];

                    duma_y += ( phi_y[p][sigma] * phi[p][nu] + phi[p][sigma] * phi_y[p][nu] ) * dap[sigma][nu];
                    dumb_y += ( phi_y[p][sigma] * phi[p][nu] + phi[p][sigma] * phi_y[p][nu] ) * dbp[sigma][nu];

                    duma_z += ( phi_z[p][sigma] * phi[p][nu] + phi[p][sigma] * phi_z[p][nu] ) * dap[sigma][nu];
                    dumb_z += ( phi_z[p][sigma] * phi[p][nu] + phi[p][sigma] * phi_z[p][nu] ) * dbp[sigma][nu];
                }
            }
            rho_a_xp[p] = duma_x;
            rho_b_xp[p] = dumb_x;

            rho_a_yp[p] = duma_y;
            rho_b_yp[p] = dumb_y;

            rho_a_zp[p] = duma_z;
            rho_b_zp[p] = dumb_z;

            sigma_aap[p] = ( rho_a_xp[p] * rho_a_xp[p] ) +  ( rho_a_yp[p] * rho_a_yp[p] ) + ( rho_a_zp[p] * rho_a_zp[p] );
            sigma_bbp[p] = ( rho_b_xp[p] * rho_b_xp[p] ) +  ( rho_b_yp[p] * rho_b_yp[p] ) + ( rho_b_zp[p] * rho_b_zp[p] );
            sigma_abp[p] = ( rho_a_xp[p] * rho_b_xp[p] ) +  ( rho_a_yp[p] * rho_b_yp[p] ) + ( rho_a_zp[p] * rho_b_zp[p] );

            // tau_ap[p] = dumta;
            // tau_bp[p] = dumtb;

        }
    }
}

void MCPDFTSolver::BuildRhoFast(opdm * D1a, opdm * D1b, int na, int nb) {

    rho_a_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    rho_b_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    rho_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // m_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    // zeta_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // rs_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    double ** phi   = super_phi_->pointer();

    double * rho_ap = rho_a_->pointer();
    double * rho_bp = rho_b_->pointer();
    double * rho_p = rho_->pointer();
    // double * m_p = m_->pointer();
    // 
    // double * zeta_p = zeta_->pointer();
    // double * rs_p = rs_->pointer();
    double temp_tot = 0.0;
    double temp_a = 0.0;
    double temp_b = 0.0;
    for (int p = 0; p < phi_points_; p++) {

        // rho_a(r)
        double duma = 0.0;
        for (int n = 0; n < na; n++) {

            int i = D1a[n].i;
            int j = D1a[n].j;

            int hi = symmetry_[i];
            int hj = symmetry_[j];

            int ii = i - pitzer_offset_[hi];
            int jj = j - pitzer_offset_[hj];

            duma += super_phi_->pointer(hi)[p][ii] *
                    super_phi_->pointer(hj)[p][jj] * D1a[n].val;

        }
        rho_ap[p] = duma;

        // rho_b(r)
        double dumb = 0.0;
        for (int n = 0; n < nb; n++) {

            int i = D1b[n].i;
            int j = D1b[n].j;

            int hi = symmetry_[i];
            int hj = symmetry_[j];

            int ii = i - pitzer_offset_[hi];
            int jj = j - pitzer_offset_[hj];

            dumb += super_phi_->pointer(hi)[p][ii] *
                    super_phi_->pointer(hj)[p][jj] * D1b[n].val;

        }
        rho_bp[p] = dumb;

        rho_p[p] = rho_ap[p] + rho_bp[p];    

        temp_tot += rho_p[p]  * grid_w_->pointer()[p];
        temp_a   += rho_ap[p] * grid_w_->pointer()[p];
        temp_b   += rho_bp[p] * grid_w_->pointer()[p];

        // rho_p[p] = rho_ap[p] + rho_bp[p];
        // m_p[p] =  rho_ap[p] - rho_bp[p];

        // zeta_p[p] =  m_p[p]  / rho_p[p];
        // rs_p[p] = pow( 3.0 / ( 4.0 * M_PI * rho_p[p] ) , 1.0/3.0 );
    }
    outfile->Printf("\n");
    outfile->Printf("      Integrated total density = %20.12lf\n",temp_tot);
    outfile->Printf("      Integrated alpha density = %20.12lf\n",temp_a);
    outfile->Printf("      Integrated beta density  = %20.12lf\n",temp_b);
    outfile->Printf("\n");

    if ( is_gga_ || is_meta_ ) {

        sigma_aa_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        sigma_bb_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        sigma_ab_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        rho_a_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        rho_b_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        rho_a_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        rho_b_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        rho_a_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        rho_b_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        // tau_a_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
        // tau_b_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));

        double ** phi_x = super_phi_x_->pointer();
        double ** phi_y = super_phi_y_->pointer();
        double ** phi_z = super_phi_z_->pointer();

        double * sigma_aap = sigma_aa_->pointer();
        double * sigma_bbp = sigma_bb_->pointer();
        double * sigma_abp = sigma_ab_->pointer();

        double * rho_a_xp = rho_a_x_->pointer();
        double * rho_b_xp = rho_b_x_->pointer();

        double * rho_a_yp = rho_a_y_->pointer();
        double * rho_b_yp = rho_b_y_->pointer();

        double * rho_a_zp = rho_a_z_->pointer();
        double * rho_b_zp = rho_b_z_->pointer();

        // double * tau_ap = tau_a_->pointer();
        // double * tau_bp = tau_b_->pointer();

        for (int p = 0; p < phi_points_; p++) {

            // rho'_a(r)

            double duma_x = 0.0;
            double duma_y = 0.0;
            double duma_z = 0.0;

            for (int n = 0; n < na; n++) {

                int i = D1a[n].i;
                int j = D1a[n].j;
                
                int hi = symmetry_[i];
                int hj = symmetry_[j];
                
                int ii = i - pitzer_offset_[hi];
                int jj = j - pitzer_offset_[hj];
                
                duma_x += ( super_phi_x_->pointer(hi)[p][ii] * super_phi_->pointer(hj)[p][jj] 
                        +   super_phi_->pointer(hi)[p][ii] * super_phi_x_->pointer(hj)[p][jj] ) * D1a[n].val;

                duma_y += ( super_phi_y_->pointer(hi)[p][ii] * super_phi_->pointer(hj)[p][jj] 
                        +   super_phi_->pointer(hi)[p][ii] * super_phi_y_->pointer(hj)[p][jj] ) * D1a[n].val;

                duma_z += ( super_phi_z_->pointer(hi)[p][ii] * super_phi_->pointer(hj)[p][jj] 
                        +   super_phi_->pointer(hi)[p][ii] * super_phi_z_->pointer(hj)[p][jj] ) * D1a[n].val;

            }

            // rho'_b(r)

            double dumb_x = 0.0;
            double dumb_y = 0.0;
            double dumb_z = 0.0;

            for (int n = 0; n < nb; n++) {
                
                int i = D1b[n].i;
                int j = D1b[n].j;
                
                int hi = symmetry_[i];
                int hj = symmetry_[j];
                
                int ii = i - pitzer_offset_[hi];
                int jj = j - pitzer_offset_[hj];
                
                dumb_x += ( super_phi_x_->pointer(hi)[p][ii] * super_phi_->pointer(hj)[p][jj] 
                        +   super_phi_->pointer(hi)[p][ii] * super_phi_x_->pointer(hj)[p][jj] ) * D1b[n].val;
                
                dumb_y += ( super_phi_y_->pointer(hi)[p][ii] * super_phi_->pointer(hj)[p][jj] 
                        +   super_phi_->pointer(hi)[p][ii] * super_phi_y_->pointer(hj)[p][jj] ) * D1b[n].val;
                
                dumb_z += ( super_phi_z_->pointer(hi)[p][ii] * super_phi_->pointer(hj)[p][jj] 
                        +   super_phi_->pointer(hi)[p][ii] * super_phi_z_->pointer(hj)[p][jj] ) * D1b[n].val;

            }

            rho_a_xp[p] = duma_x;
            rho_b_xp[p] = dumb_x;

            rho_a_yp[p] = duma_y;
            rho_b_yp[p] = dumb_y;

            rho_a_zp[p] = duma_z;
            rho_b_zp[p] = dumb_z;

            sigma_aap[p] = ( rho_a_xp[p] * rho_a_xp[p] ) +  ( rho_a_yp[p] * rho_a_yp[p] ) + ( rho_a_zp[p] * rho_a_zp[p] );
            sigma_bbp[p] = ( rho_b_xp[p] * rho_b_xp[p] ) +  ( rho_b_yp[p] * rho_b_yp[p] ) + ( rho_b_zp[p] * rho_b_zp[p] );
            sigma_abp[p] = ( rho_a_xp[p] * rho_b_xp[p] ) +  ( rho_a_yp[p] * rho_b_yp[p] ) + ( rho_a_zp[p] * rho_b_zp[p] );

            // tau_ap[p] = dumta;
            // tau_bp[p] = dumtb;

        }
    }
}

std::vector< std::shared_ptr<Matrix> > MCPDFTSolver::BuildJ() {

    // get primary basis:
    std::shared_ptr<BasisSet> primary = reference_wavefunction_->get_basisset("ORBITAL");

    std::vector< std::shared_ptr<Matrix> > J;

    // JK object (note this is hard-coded to use density fitting ...)
    if (options_.get_str("MCPDFT_TYPE") == "DF") {

         // get auxiliary basis:
         std::shared_ptr<BasisSet> auxiliary = reference_wavefunction_->get_basisset("DF_BASIS_SCF");
        
        // outfile->Printf("\n");
        // outfile->Printf("    ==> The JK type for the MCPDFT calculation is: %s", options_.get_str("MCPDFT_TYPE"));
        // outfile->Printf(" <==\n");

        std::shared_ptr<DFJK> jk = (std::shared_ptr<DFJK>)(new DFJK(primary,auxiliary));
        
        // memory for jk (say, 50% of what is available)
        jk->set_memory(0.5 * Process::environment.get_memory());

        // integral cutoff
        jk->set_cutoff(options_.get_double("INTS_TOLERANCE"));

        jk->set_do_J(true);
        jk->set_do_K(false);
        jk->set_do_wK(false);
        jk->set_omega(false);

        jk->initialize();

        std::vector<SharedMatrix>& C_left  = jk->C_left();
        std::vector<SharedMatrix>& C_right = jk->C_right();

        // use alpha and beta C matrices

        std::shared_ptr<Matrix> myCa (new Matrix(Ca_) );
        std::shared_ptr<Matrix> myCb (new Matrix(Cb_) );

        C_left.clear();
        C_right.clear();

        // alpha first 
        myCa->zero();
        myCa->gemm('t','n',1.0,Da_,Ca_,0.0);
        myCa->transpose_this();
        C_left.push_back(myCa);
        C_right.push_back(Ca_);

        // beta second 
        myCb->zero();
        myCb->gemm('t','n',1.0,Db_,Cb_,0.0);
        myCb->transpose_this();
        C_left.push_back(myCb);
        C_right.push_back(Cb_);

        // Let jk compute for the given C_left/C_right

        jk->compute();

        std::shared_ptr<Matrix> Ja = jk->J()[0];
        std::shared_ptr<Matrix> Jb = jk->J()[1];

        Ja->transform(Ca_);
        Jb->transform(Cb_);

        J.push_back(Ja);
        J.push_back(Jb);

        return J;

    }else if (options_.get_str("MCPDFT_TYPE") == "PK") {

        // outfile->Printf("\n");
        // outfile->Printf("    ==> The JK type for the MCPDFT calculation is: %s", options_.get_str("MCPDFT_TYPE"));
        // outfile->Printf(" <==\n");

        std::shared_ptr<PKJK> jk = (std::shared_ptr<PKJK>)(new PKJK(primary,options_));

        // memory for jk (say, 50% of what is available)
        jk->set_memory(0.5 * Process::environment.get_memory());

        // integral cutoff
        jk->set_cutoff(options_.get_double("INTS_TOLERANCE"));

        jk->set_do_J(true);
        jk->set_do_K(false);
        jk->set_do_wK(false);
        jk->set_omega(false);

        jk->initialize();

        std::vector<SharedMatrix>& C_left  = jk->C_left();
        std::vector<SharedMatrix>& C_right = jk->C_right();

        // use alpha and beta C matrices

        std::shared_ptr<Matrix> myCa (new Matrix(Ca_) );
        std::shared_ptr<Matrix> myCb (new Matrix(Cb_) );

        C_left.clear();
        C_right.clear();

        // alpha first 
        myCa->zero();
        myCa->gemm('t','n',1.0,Da_,Ca_,0.0);
        myCa->transpose_this();
        C_left.push_back(myCa);
        C_right.push_back(Ca_);

        // beta second 
        myCb->zero();
        myCb->gemm('t','n',1.0,Db_,Cb_,0.0);
        myCb->transpose_this();
        C_left.push_back(myCb);
        C_right.push_back(Cb_);

        // Let jk compute for the given C_left/C_right

        jk->compute();

        std::shared_ptr<Matrix> Ja = jk->J()[0];
        std::shared_ptr<Matrix> Jb = jk->J()[1];

        Ja->transform(Ca_);
        Jb->transform(Cb_);

        J.push_back(Ja);
        J.push_back(Jb);

        return J;
        
    }else {
        throw PsiException("invalid MCSCF_TYPE",__FILE__,__LINE__);
    }
}

void MCPDFTSolver::Build_R(){

    double tol = 1.0e-20;

    R_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    
    double * R_p = R_->pointer();
    double * rho_p = rho_->pointer();
    double * pi_p = pi_->pointer();
 
    for (int p = 0; p < phi_points_; p++) {
        
        R_p[p] = 4 * pi_p[p] / (rho_p[p] * rho_p[p]); 
    }
} 

void MCPDFTSolver::Translate(){

    double tol = 1.0e-20;

    tr_rho_a_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    tr_rho_b_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    double * tr_rho_ap = tr_rho_a_->pointer();
    double * tr_rho_bp = tr_rho_b_->pointer();

    double * rho_p = rho_->pointer();
    double * pi_p = pi_->pointer();
    double * R_p = R_->pointer();

    // if ( deriv_ != 0) {

    // tr_rho_a_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // tr_rho_b_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    // tr_rho_a_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // tr_rho_b_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

    // tr_rho_a_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // tr_rho_b_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    // 
    // double * rho_a_xp = rho_a_x_->pointer();
    // double * rho_b_xp = rho_b_x_->pointer();

    // double * rho_a_yp = rho_a_y_->pointer();
    // double * rho_b_yp = rho_b_y_->pointer();

    // double * rho_a_zp = rho_a_z_->pointer();
    // double * rho_b_zp = rho_b_z_->pointer();

    // double * pi_xp = pi_x_->pointer();
    // double * pi_yp = pi_y_->pointer();
    // double * pi_zp = pi_z_->pointer();

    // double * tr_rho_a_xp = tr_rho_a_x_->pointer();
    // double * tr_rho_b_xp = tr_rho_b_x_->pointer();

    // double * tr_rho_a_yp = tr_rho_a_y_->pointer();
    // double * tr_rho_b_yp = tr_rho_b_y_->pointer();

    // double * tr_rho_a_zp = tr_rho_a_z_->pointer();
    // double * tr_rho_b_zp = tr_rho_b_z_->pointer();

    // }

    double temp_tot = 0.0;
    double temp_a = 0.0;
    double temp_b = 0.0;
    for (int p = 0; p < phi_points_; p++) {

        double rho = rho_p[p];
        double pi = pi_p[p];
        
        double zeta = 0.0;
        double R = 0.0;

	if ( !(rho < tol) && !(pi < tol) ) {

           R = R_p[p];
           // R = tanh(R);
           // outfile->Printf("tanh(R) = %12.5lf\n",R);

           if ( (1.0 - R) > tol ) {

              zeta = sqrt(1.0 - R);

           }else{

                zeta = 0.0;
           }

           tr_rho_ap[p] = (1.0 + zeta) * (rho/2.0);
           tr_rho_bp[p] = (1.0 - zeta) * (rho/2.0);
           // tr_rs_p[p] = pow( 3.0 / ( 4.0 * M_PI * (tr_rho_ap[p] + tr_rho_bp[p]) ) , 1.0/3.0 );

        }else {

               tr_rho_ap[p] = 0.0;
               tr_rho_bp[p] = 0.0;
        }

        temp_tot += (tr_rho_ap[p] + tr_rho_bp[p]) * grid_w_->pointer()[p];
        temp_a += tr_rho_ap[p] * grid_w_->pointer()[p];
        temp_b += tr_rho_bp[p] * grid_w_->pointer()[p];

        // outfile->Printf("zeta = %12.15lf\n",tr_zeta_p[p]);
        // outfile->Printf("rs = %12.15lf\n",tr_rs_p[p]);
        
        // tr_m_p[p] = (R_p[p] > 1.0) ? 0.0 : rho_p[p] * sqrt(1.0 - R_p[p]);

        // tr_rho_ap[p] = ( (R_p[p] > 1.0) ? (rho/2.0) : (rho/2.0) * ( 1.0 + sqrt(1.0 - R_p[p]) ) );
        // tr_rho_bp[p] = ( (R_p[p] > 1.0) ? (rho/2.0) : (rho/2.0) * ( 1.0 - sqrt(1.0 - R_p[p]) ) );

        // tr_zeta_p[p] =  ( tr_rho_ap[p] - tr_rho_bp[p] ) / ( tr_rho_ap[p] + tr_rho_bp[p] );
        // tr_rs_p[p] = pow( 3.0 / ( 4.0 * M_PI * (tr_rho_ap[p] + tr_rho_bp[p]) ) , 1.0/3.0 );
    }

    outfile->Printf("\n");
    outfile->Printf("      Integrated translated total density = %20.12lf\n",temp_tot);
    outfile->Printf("      Integrated translated alpha density = %20.12lf\n",temp_a);
    outfile->Printf("      Integrated translated beta density  = %20.12lf\n",temp_b);
    outfile->Printf("\n");

    if ( is_gga_ || is_meta_ ) {

       tr_rho_a_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_rho_b_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

       tr_rho_a_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_rho_b_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

       tr_rho_a_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_rho_b_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

       tr_sigma_aa_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_sigma_ab_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_sigma_bb_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

       double * rho_a_xp = rho_a_x_->pointer();
       double * rho_b_xp = rho_b_x_->pointer();

       double * rho_a_yp = rho_a_y_->pointer();
       double * rho_b_yp = rho_b_y_->pointer();

       double * rho_a_zp = rho_a_z_->pointer();
       double * rho_b_zp = rho_b_z_->pointer();

       // double * pi_xp = pi_x_->pointer();
       // double * pi_yp = pi_y_->pointer();
       // double * pi_zp = pi_z_->pointer();

       double * tr_rho_a_xp = tr_rho_a_x_->pointer();
       double * tr_rho_b_xp = tr_rho_b_x_->pointer();

       double * tr_rho_a_yp = tr_rho_a_y_->pointer();
       double * tr_rho_b_yp = tr_rho_b_y_->pointer();

       double * tr_rho_a_zp = tr_rho_a_z_->pointer();
       double * tr_rho_b_zp = tr_rho_b_z_->pointer();

       double * tr_sigma_aap = tr_sigma_aa_->pointer();
       double * tr_sigma_abp = tr_sigma_ab_->pointer();
       double * tr_sigma_bbp = tr_sigma_bb_->pointer();

       for (int p = 0; p < phi_points_; p++) {

           double rho = rho_p[p];
           double pi = pi_p[p];

           double rho_x = rho_a_xp[p] + rho_b_xp[p];
           double rho_y = rho_a_yp[p] + rho_b_yp[p];
           double rho_z = rho_a_zp[p] + rho_b_zp[p];
           
           double zeta = 0.0;
           double R = 0.0;

           if ( !(rho < tol) && !(pi < tol) ) {

              R = R_p[p];
              // R = tanh(R);
               
              if ( (1.0 - R) > tol )  {

                 zeta = sqrt(1.0 - R);

                 // tr_rho_a_xp[p] =  zeta_x * (rho/2.0) + (1.0 + zeta) * (rho_x/2.0);
                 // tr_rho_b_xp[p] = -zeta_x * (rho/2.0) + (1.0 - zeta) * (rho_x/2.0);

                 // tr_rho_a_yp[p] =  zeta_y * (rho/2.0) + (1.0 + zeta) * (rho_y/2.0);
                 // tr_rho_b_yp[p] = -zeta_y * (rho/2.0) + (1.0 - zeta) * (rho_y/2.0);

                 // tr_rho_a_zp[p] =  zeta_z * (rho/2.0) + (1.0 + zeta) * (rho_z/2.0);
                 // tr_rho_b_zp[p] = -zeta_z * (rho/2.0) + (1.0 - zeta) * (rho_z/2.0);

              }else{

                   zeta = 0.0;
              }

              tr_rho_a_xp[p] = (1.0 + zeta) * (rho_x/2.0);
              tr_rho_b_xp[p] = (1.0 - zeta) * (rho_x/2.0);

              tr_rho_a_yp[p] = (1.0 + zeta) * (rho_y/2.0);
              tr_rho_b_yp[p] = (1.0 - zeta) * (rho_y/2.0);

              tr_rho_a_zp[p] = (1.0 + zeta) * (rho_z/2.0);
              tr_rho_b_zp[p] = (1.0 - zeta) * (rho_z/2.0);

           }else {

                 tr_rho_a_xp[p] = 0.0;
                 tr_rho_b_xp[p] = 0.0;

                 tr_rho_a_yp[p] = 0.0;
                 tr_rho_b_yp[p] = 0.0;

                 tr_rho_a_zp[p] = 0.0;
                 tr_rho_b_zp[p] = 0.0;
           }

           tr_sigma_aap[p] = (tr_rho_a_xp[p] * tr_rho_a_xp[p]) + (tr_rho_a_yp[p] * tr_rho_a_yp[p]) + (tr_rho_a_zp[p] * tr_rho_a_zp[p]);
           tr_sigma_abp[p] = (tr_rho_a_xp[p] * tr_rho_b_xp[p]) + (tr_rho_a_yp[p] * tr_rho_b_yp[p]) + (tr_rho_a_zp[p] * tr_rho_b_zp[p]);
           tr_sigma_bbp[p] = (tr_rho_b_xp[p] * tr_rho_b_xp[p]) + (tr_rho_b_yp[p] * tr_rho_b_yp[p]) + (tr_rho_b_zp[p] * tr_rho_b_zp[p]);
       }
    }
}

void MCPDFTSolver::Fully_Translate(){
    
    double tol = 1.0e-20;

    double const R0 = 0.9;
    double const R1 = 1.15;
    double const A = -475.60656009;
    double const B = -379.47331922;
    double const C = -85.38149682;

    tr_rho_a_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    tr_rho_b_   = (std::shared_ptr<Vector>)(new Vector(phi_points_));
    
    double * tr_rho_ap = tr_rho_a_->pointer();
    double * tr_rho_bp = tr_rho_b_->pointer();
    
    double * rho_p = rho_->pointer();
    double * pi_p = pi_->pointer();
    double * R_p = R_->pointer();

    double temp_tot = 0.0;
    double temp_a = 0.0;
    double temp_b = 0.0;
    for (int p = 0; p < phi_points_; p++) {

        double rho = rho_p[p];
        double pi = pi_p[p];
        double DelR = R_p[p] - R1;

        double zeta = 0.0;
        double R = 0.0;

	if ( !(rho < tol) && !(pi < tol) ) {

           R = R_p[p];
           // R = tanh(R);

           if ( ((1.0 - R) > tol) && ( R < R0 ) ) {

              zeta = sqrt(1.0 - R);

           }else if( !(R < R0) && !(R > R1) ) {

                   zeta = A * pow(DelR, 5.0) + B * pow(DelR, 4.0) + C * pow(DelR, 3.0);

           }else if( R > R1 ) {

                   zeta = 0.0;
           }

           tr_rho_ap[p] = (1.0 + zeta) * (rho/2.0);
           tr_rho_bp[p] = (1.0 - zeta) * (rho/2.0);

        }else{

             tr_rho_ap[p] = 0.0;
             tr_rho_bp[p] = 0.0;
        }

        temp_a += tr_rho_ap[p] * grid_w_->pointer()[p];
        temp_b += tr_rho_bp[p] * grid_w_->pointer()[p];
        temp_tot += ( tr_rho_bp[p] + tr_rho_ap[p] ) * grid_w_->pointer()[p];

    }

    outfile->Printf("\n");
    outfile->Printf("      Integrated fully translated total density = %20.12lf\n",temp_tot);
    outfile->Printf("      Integrated fully translated alpha density = %20.12lf\n",temp_a);
    outfile->Printf("      Integrated fully translated beta density  = %20.12lf\n",temp_b);
    outfile->Printf("\n");

    if ( is_gga_ || is_meta_ ) {

       tr_rho_a_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_rho_b_x_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

       tr_rho_a_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_rho_b_y_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

       tr_rho_a_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_rho_b_z_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       
       tr_sigma_aa_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_sigma_ab_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));
       tr_sigma_bb_ = (std::shared_ptr<Vector>)(new Vector(phi_points_));

       double * tr_rho_a_xp = tr_rho_a_x_->pointer();
       double * tr_rho_b_xp = tr_rho_b_x_->pointer();

       double * tr_rho_a_yp = tr_rho_a_y_->pointer();
       double * tr_rho_b_yp = tr_rho_b_y_->pointer();
 
       double * tr_rho_a_zp = tr_rho_a_z_->pointer();
       double * tr_rho_b_zp = tr_rho_b_z_->pointer();
       
       double * tr_sigma_aap = tr_sigma_aa_->pointer();
       double * tr_sigma_abp = tr_sigma_ab_->pointer();
       double * tr_sigma_bbp = tr_sigma_bb_->pointer();

       double * rho_a_xp = rho_a_x_->pointer();
       double * rho_b_xp = rho_b_x_->pointer();

       double * rho_a_yp = rho_a_y_->pointer();
       double * rho_b_yp = rho_b_y_->pointer();
 
       double * rho_a_zp = rho_a_z_->pointer();
       double * rho_b_zp = rho_b_z_->pointer();

       double * pi_xp = pi_x_->pointer();
       double * pi_yp = pi_y_->pointer();
       double * pi_zp = pi_z_->pointer();

       for (int p = 0; p < phi_points_; p++) {

           double rho_x = rho_a_xp[p] + rho_b_xp[p];
           double rho_y = rho_a_yp[p] + rho_b_yp[p];
           double rho_z = rho_a_zp[p] + rho_b_zp[p];

           double rho = rho_p[p];
           double pi = pi_p[p];
           double DelR = R_p[p] - R1;

           double zeta = 0.0;
           double R = 0.0;
 
           if ( !(rho < tol) && !(pi < tol) ) {
           
               R = R_p[p];
               // R = tanh(R);
           
               if ( ((1.0 - R) > tol) && ( R < R0 ) ) {
           
                  zeta = sqrt(1.0 - R);
           
                  tr_rho_a_xp[p] = (1.0 + zeta) * (rho_x/2.0) + (R * rho_x) / (2.0*zeta) - pi_xp[p] / (rho*zeta);
                  tr_rho_b_xp[p] = (1.0 - zeta) * (rho_x/2.0) - (R * rho_x) / (2.0*zeta) + pi_xp[p] / (rho*zeta);
                  
                  tr_rho_a_yp[p] = (1.0 + zeta) * (rho_y/2.0) + (R * rho_y) / (2.0*zeta) - pi_yp[p] / (rho*zeta);
                  tr_rho_b_yp[p] = (1.0 - zeta) * (rho_y/2.0) - (R * rho_y) / (2.0*zeta) + pi_yp[p] / (rho*zeta);

                  tr_rho_a_zp[p] = (1.0 + zeta) * (rho_z/2.0) + (R * rho_z) / (2.0*zeta) - pi_zp[p] / (rho*zeta);
                  tr_rho_b_zp[p] = (1.0 - zeta) * (rho_z/2.0) - (R * rho_z) / (2.0*zeta) + pi_zp[p] / (rho*zeta);

          }else if( !(R < R0) && !(R > R1) ) {
       
                  zeta = A * pow(DelR, 5.0) + B * pow(DelR, 4.0) + C * pow(DelR, 3.0);

                       tr_rho_a_xp[p] = (1.0 + zeta) * (rho_x/2.0) 
                                       + (A * pow(DelR, 4.0)) * ( (10.0 * pi_xp[p] / rho) - (5.0 * R * rho_x) )
                                       + (B * pow(DelR, 3.0)) * ( (8.0  * pi_xp[p] / rho) - (4.0 * R * rho_x) )
                                       + (C * pow(DelR, 2.0)) * ( (6.0  * pi_xp[p] / rho) - (3.0 * R * rho_x) );

                       tr_rho_b_xp[p] = (1.0 - zeta) * (rho_x/2.0) 
                                       + (A * pow(DelR, 4.0)) * (-(10.0 * pi_xp[p] / rho) + (5.0 * R * rho_x) )
                                       + (B * pow(DelR, 3.0)) * (-(8.0  * pi_xp[p] / rho) + (4.0 * R * rho_x) )
                                       + (C * pow(DelR, 2.0)) * (-(6.0  * pi_xp[p] / rho) + (3.0 * R * rho_x) );
           
                       tr_rho_a_yp[p] = (1.0 + zeta) * (rho_y/2.0) 
                                       + (A * pow(DelR, 4.0)) * ( (10.0 * pi_yp[p] / rho) - (5.0 * R * rho_y) )
                                       + (B * pow(DelR, 3.0)) * ( (8.0  * pi_yp[p] / rho) - (4.0 * R * rho_y) )
                                       + (C * pow(DelR, 2.0)) * ( (6.0  * pi_yp[p] / rho) - (3.0 * R * rho_y) );

                       tr_rho_b_yp[p] = (1.0 - zeta) * (rho_y/2.0) 
                                       + (A * pow(DelR, 4.0)) * (-(10.0 * pi_yp[p] / rho) + (5.0 * R * rho_y) )
                                       + (B * pow(DelR, 3.0)) * (-(8.0  * pi_yp[p] / rho) + (4.0 * R * rho_y) )
                                       + (C * pow(DelR, 2.0)) * (-(6.0  * pi_yp[p] / rho) + (3.0 * R * rho_y) );

                       tr_rho_a_zp[p] = (1.0 + zeta) * (rho_z/2.0) 
                                       + (A * pow(DelR, 4.0)) * ( (10.0 * pi_zp[p] / rho) - (5.0 * R * rho_z) )
                                       + (B * pow(DelR, 3.0)) * ( (8.0  * pi_zp[p] / rho) - (4.0 * R * rho_z) )
                                       + (C * pow(DelR, 2.0)) * ( (6.0  * pi_zp[p] / rho) - (3.0 * R * rho_z) );

                       tr_rho_b_zp[p] = (1.0 - zeta) * (rho_z/2.0) 
                                       + (A * pow(DelR, 4.0)) * (-(10.0 * pi_zp[p] / rho) + (5.0 * R * rho_z) )
                                       + (B * pow(DelR, 3.0)) * (-(8.0  * pi_zp[p] / rho) + (4.0 * R * rho_z) )
                                       + (C * pow(DelR, 2.0)) * (-(6.0  * pi_zp[p] / rho) + (3.0 * R * rho_z) );
               }else if( R > R1 ) {
           
                       zeta = 0.0;

                       tr_rho_a_xp[p] = (1.0 + zeta) * (rho_x/2.0);
                       tr_rho_b_xp[p] = (1.0 - zeta) * (rho_x/2.0);

                       tr_rho_a_yp[p] = (1.0 + zeta) * (rho_y/2.0);
                       tr_rho_b_yp[p] = (1.0 - zeta) * (rho_y/2.0);

                       tr_rho_a_zp[p] = (1.0 + zeta) * (rho_z/2.0);
                       tr_rho_b_zp[p] = (1.0 - zeta) * (rho_z/2.0);
               }
           
           ftr_rho_a_yp[p] = 0.0;
           ftr_rho_b_yp[p] = 0.0;
           
               tr_rho_a_xp[p] = 0.0;
               tr_rho_b_xp[p] = 0.0;
               
               tr_rho_a_yp[p] = 0.0;
               tr_rho_b_yp[p] = 0.0;
               
               tr_rho_a_zp[p] = 0.0;
               tr_rho_b_zp[p] = 0.0;
           }
           // outfile->Printf("\n     Fully translated density gradient (x-component) =      %12.15lf\n",tr_rho_a_xp[p]);

           tr_sigma_aap[p] = (tr_rho_a_xp[p] * tr_rho_a_xp[p]) + (tr_rho_a_yp[p] * tr_rho_a_yp[p]) + (tr_rho_a_zp[p] * tr_rho_a_zp[p]);  
           tr_sigma_abp[p] = (tr_rho_a_xp[p] * tr_rho_b_xp[p]) + (tr_rho_a_yp[p] * tr_rho_b_yp[p]) + (tr_rho_a_zp[p] * tr_rho_b_zp[p]);  
           tr_sigma_bbp[p] = (tr_rho_b_xp[p] * tr_rho_b_xp[p]) + (tr_rho_b_yp[p] * tr_rho_b_yp[p]) + (tr_rho_b_zp[p] * tr_rho_b_zp[p]);  

void MCPDFTSolver::ReadCITPDM(double* D, const char* fileName) {
    
    std::ifstream dataIn;
    
    dataIn.open(fileName);
    
    if (!dataIn)
       std::cout << "Error opening file.\n";
    else { 
         for (int i = 0; i < nmo_; i++)
             for (int j = 0; j < nmo_; j++)
                 for (int k = 0; k < nmo_; k++)
                     for (int l = 0; l < nmo_; l++) {

                         dataIn >> D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l];
                         if (D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l] < 1e-20)
                             D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l] = 0.0;
                     }
    dataIn.close(); 
    }
}

void MCPDFTSolver::PrintTPDM(double* D) {
    
   for (int i = 0; i < nmo_; i++)
       for (int j = 0; j < nmo_; j++)
           for (int k = 0; k < nmo_; k++)
               for (int l = 0; l < nmo_; l++)
                   printf("%20.15lf\t", D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l]);
                         
   
   printf("\n\n");
}

void MCPDFTSolver::ReadCIOPDM(double* D, const char* fileName) {
    
    std::ifstream dataIn;
    
    dataIn.open(fileName);
    
    if (!dataIn)
       std::cout << "Error opening file.\n";
    else { 
         for (int i = 0; i < nmo_; i++)
             for (int j = 0; j < nmo_; j++) {
                 
                 dataIn >> D[i*nmo_+j];
                 if (D[i*nmo_+j] < 1.e-20)
                     D[i*nmo_+j] = 0.0;
             }        
    dataIn.close(); 
    }
}

// void MCPDFTSolver::ReadTPDM(double* D, const char* fileName) {
//     
//     double tol = 1.0e-20;
// 
//     std::ifstream dataIn;
//     
//     dataIn.open(fileName);
//     
//     if (!dataIn)
//        std::cout << "Error opening file.\n";
//     else { 
//          for (int i = 0; i < nmo_; i++)
//              for (int j = 0; j < nmo_; j++)
//                  for (int k = 0; k < nmo_; k++)
//                      for (int l = 0; l < nmo_; l++) {
// 
//                          dataIn >> D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l];
//                          if (D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l] < tol)
//                              D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l] = 0.0;
//                      }
//     dataIn.close(); 
//     }
// }
// 
// void MCPDFTSolver::PrintTPDM(double* D) {
//     
//    for (int i = 0; i < nmo_; i++)
//        for (int j = 0; j < nmo_; j++)
//            for (int k = 0; k < nmo_; k++)
//                for (int l = 0; l < nmo_; l++)
//                    outfile->Printf("%20.15lf\t", D[i*nmo_*nmo_*nmo_+j*nmo_*nmo_+k*nmo_+l]);
//                          
//    
//    printf("\n\n");
// }
// 
// void MCPDFTSolver::ReadOPDM(double* D, const char* fileName) {
//    
//     double tol = 1.0e-20;
//   
//     std::ifstream dataIn;
//     
//     dataIn.open(fileName);
//     
//     if (!dataIn)
//        std::cout << "Error opening file.\n";
//     else { 
//          for (int i = 0; i < nmo_; i++)
//              for (int j = 0; j < nmo_; j++) {
//                  
//                  dataIn >> D[i*nmo_+j];
//                  if (D[i*nmo_+j] < tol)
//                      D[i*nmo_+j] = 0.0;
//              }        
//     dataIn.close(); 
//     }
// }
// 
// void MCPDFTSolver::PrintOPDM(double* D) {
//     
//    for (int i = 0; i < nmo_; i++)
//        for (int j = 0; j < nmo_; j++)
//            printf("%20.15lf\t", D[i*nmo_+j]);
//                      
//    
//    printf("\n\n");
// }

}} // end of namespaces



