/*
  Copyright 2013, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2015 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2015 NTNU
  Copyright 2015 IRIS AS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_NEWTONSOLVER_IMPL_HEADER_INCLUDED
#define OPM_NEWTONSOLVER_IMPL_HEADER_INCLUDED

#include <opm/autodiff/NewtonSolver.hpp>

namespace Opm
{
    template <class PhysicalModel>
    NewtonSolver<PhysicalModel>::NewtonSolver(const SolverParameters& param,
                                              std::unique_ptr<PhysicalModel> model)
        : param_(param),
          model_(std::move(model)),
          newtonIterations_(0),
          linearIterations_(0)
    {
    }

    template <class PhysicalModel>
    unsigned int NewtonSolver<PhysicalModel>::newtonIterations () const
    {
        return newtonIterations_;
    }

    template <class PhysicalModel>
    unsigned int NewtonSolver<PhysicalModel>::linearIterations () const
    {
        return linearIterations_;
    }


    template <class PhysicalModel>
    int
    NewtonSolver<PhysicalModel>::
    step(const double dt,
         ReservoirState& reservoir_state,
         WellState& well_state)
    {
        // Do model-specific once-per-step calculations.
        model_->prepareStep(dt, reservoir_state, well_state);

        // For each iteration we store in a vector the norms of the residual of
        // the mass balance for each active phase, the well flux and the well equations.
        std::vector<std::vector<double>> residual_norms_history;

        // Assemble residual and Jacobian, store residual norms.
        model_->assemble(reservoir_state, well_state, true);
        residual_norms_history.push_back(model_->computeResidualNorms());

        // Set up for main Newton loop.
        double omega = 1.0;
        int iteration = 0;
        bool converged = model_->getConvergence(dt, iteration);
        const int sizeNonLinear = model_->sizeNonLinear();
        V dxOld = V::Zero(sizeNonLinear);
        bool isOscillate = false;
        bool isStagnate = false;
        const enum RelaxType relaxtype = relaxType();
        int linearIterations = 0;

        // ----------  Main Newton loop  ----------
        while ( (!converged && (iteration < maxIter())) || (minIter() > iteration)) {
            // Compute the Newton update to the primary variables.
            V dx = model_->solveJacobianSystem();

            // Store number of linear iterations used.
            linearIterations += model_->linearIterationsLastSolve();

            // Stabilize the Newton update.
            detectNewtonOscillations(residual_norms_history, iteration, relaxRelTol(), isOscillate, isStagnate);
            if (isOscillate) {
                omega -= relaxIncrement();
                omega = std::max(omega, relaxMax());
                if (model_->terminalOutputEnabled()) {
                    std::cout << " Oscillating behavior detected: Relaxation set to " << omega << std::endl;
                }
            }
            stabilizeNewton(dx, dxOld, omega, relaxtype);

            // Apply the update, the model may apply model-dependent
            // limitations and chopping of the update.
            model_->updateState(dx, reservoir_state, well_state);

            // Assemble residual and Jacobian, store residual norms.
            model_->assemble(reservoir_state, well_state, false);
            residual_norms_history.push_back(model_->computeResidualNorms());

            // increase iteration counter
            ++iteration;

            converged = model_->getConvergence(dt, iteration);
        }

        if (!converged) {
            if (model_->terminalOutputEnabled()) {
                std::cerr << "WARNING: Failed to compute converged solution in " << iteration << " iterations." << std::endl;
            }
            return -1; // -1 indicates that the solver has to be restarted
        }

        linearIterations_ += linearIterations;
        newtonIterations_ += iteration;
        linearIterationsLast_ = linearIterations;
        newtonIterationsLast_ = iteration;

        // Do model-specific post-step actions.
        model_->afterStep(dt, reservoir_state, well_state);

        return linearIterations;
    }



    template <class PhysicalModel>
    void NewtonSolver<PhysicalModel>::SolverParameters::
    reset()
    {
        // default values for the solver parameters
        relax_type_      = DAMPEN;
        relax_max_       = 0.5;
        relax_increment_ = 0.1;
        relax_rel_tol_   = 0.2;
        max_iter_        = 15;
        min_iter_        = 1;
    }

    template <class PhysicalModel>
    NewtonSolver<PhysicalModel>::SolverParameters::
    SolverParameters()
    {
        // set default values
        reset();
    }

    template <class PhysicalModel>
    NewtonSolver<PhysicalModel>::SolverParameters::
    SolverParameters( const parameter::ParameterGroup& param )
    {
        // set default values
        reset();

        // overload with given parameters
        relax_max_   = param.getDefault("relax_max", relax_max_);
        max_iter_    = param.getDefault("max_iter", max_iter_);
        min_iter_    = param.getDefault("min_iter", min_iter_);

        std::string relaxation_type = param.getDefault("relax_type", std::string("dampen"));
        if (relaxation_type == "dampen") {
            relax_type_ = DAMPEN;
        } else if (relaxation_type == "sor") {
            relax_type_ = SOR;
        } else {
            OPM_THROW(std::runtime_error, "Unknown Relaxtion Type " << relaxation_type);
        }
    }

    template <class PhysicalModel>
    void
    NewtonSolver<PhysicalModel>::detectNewtonOscillations(const std::vector<std::vector<double>>& residual_history,
                                                          const int it, const double relaxRelTol,
                                                          bool& oscillate, bool& stagnate) const
    {
        // The detection of oscillation in two primary variable results in the report of the detection
        // of oscillation for the solver.
        // Only the saturations are used for oscillation detection for the black oil model.
        // Stagnate is not used for any treatment here.

        if ( it < 2 ) {
            oscillate = false;
            stagnate = false;
            return;
        }

        stagnate = true;
        int oscillatePhase = 0;
        const std::vector<double>& F0 = residual_history[it];
        const std::vector<double>& F1 = residual_history[it - 1];
        const std::vector<double>& F2 = residual_history[it - 2];
        for (int p= 0; p < model_->numPhases(); ++p){
            const double d1 = std::abs((F0[p] - F2[p]) / F0[p]);
            const double d2 = std::abs((F0[p] - F1[p]) / F0[p]);

            oscillatePhase += (d1 < relaxRelTol) && (relaxRelTol < d2);

            // Process is 'stagnate' unless at least one phase
            // exhibits significant residual change.
            stagnate = (stagnate && !(std::abs((F1[p] - F2[p]) / F2[p]) > 1.0e-3));
        }

        oscillate = (oscillatePhase > 1);
    }


    template <class PhysicalModel>
    void
    NewtonSolver<PhysicalModel>::stabilizeNewton(V& dx, V& dxOld, const double omega,
                                                 const RelaxType relax_type) const
    {
        // The dxOld is updated with dx.
        // If omega is equal to 1., no relaxtion will be appiled.

        const V tempDxOld = dxOld;
        dxOld = dx;

        switch (relax_type) {
            case DAMPEN:
                if (omega == 1.) {
                    return;
                }
                dx = dx*omega;
                return;
            case SOR:
                if (omega == 1.) {
                    return;
                }
                dx = dx*omega + (1.-omega)*tempDxOld;
                return;
            default:
                OPM_THROW(std::runtime_error, "Can only handle DAMPEN and SOR relaxation type.");
        }

        return;
    }


} // namespace Opm


#endif // OPM_FULLYIMPLICITSOLVER_IMPL_HEADER_INCLUDED
