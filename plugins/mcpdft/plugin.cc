/*
 * @BEGIN LICENSE
 *
 * mcpdft by Psi4 Developer, a plugin to:
 *
 * Psi4: an open-source quantum chemistry software package
 *
 * Copyright (c) 2007-2017 The Psi4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This file is part of Psi4.
 *
 * Psi4 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * Psi4 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with Psi4; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * @END LICENSE
 */

#include "psi4/psi4-dec.h"
#include "psi4/libpsi4util/PsiOutStream.h"
#include "psi4/liboptions/liboptions.h"
#include "psi4/libmints/wavefunction.h"
#include "psi4/libpsio/psio.hpp"
#include "psi4/libpsio/psio.hpp"
#include <psi4/libpsi4util/process.h>
#include "mcpdft_solver.h"

namespace psi{ namespace mcpdft {

extern "C"
int read_options(std::string name, Options& options)
{
    if (name == "MCPDFT"|| options.read_globals()) {
        /*- The amount of information printed to the output file -*/
        options.add_int("PRINT", 1);
        /*- MCPDFT functional -*/
        options.add_str("MCPDFT_FUNCTIONAL", "SVWN", "SVWN PBE BOP");
        /*- reference must be UKS -*/
        options.add_str("REFERENCE", "UKS");
        /*- type of density and density gradient translation:
        REGULAR = The gradients of on-top density are not considered in the polarization factor zeta
        FULL = The gradients of on-top density is included in the polarization factor zeta       -*/
        options.add_str("TRANSLATION_TYPE","","REGULAR FULL");
    }

    return true;
}

extern "C"
SharedWavefunction mcpdft(SharedWavefunction ref_wfn, Options& options)
{

    outfile->Printf("\n\n");
    outfile->Printf( "        *************************************************************\n");
    outfile->Printf( "        *                                                           *\n");
    outfile->Printf( "        *    MCPDFT                                                 *\n");
    outfile->Printf( "        *                                                           *\n");
    outfile->Printf( "        *    Multiconfigurational pair density functional theory    *\n");
    outfile->Printf( "        *                                                           *\n");
    outfile->Printf( "        *************************************************************\n");
    outfile->Printf("\n\n");

    std::shared_ptr<MCPDFTSolver> dft (new MCPDFTSolver(ref_wfn,options));
    double energy = dft->compute_energy(); 

    Process::environment.globals["CURRENT ENERGY"] = energy;
    // TODO: return mcpdft wave function instead of reference
    return ref_wfn;
}

}} // End namespaces

