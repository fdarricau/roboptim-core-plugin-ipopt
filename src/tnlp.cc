// Copyright (C) 2009 by Thomas Moulard, AIST, CNRS, INRIA.
//
// This file is part of the roboptim.
//
// roboptim is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// roboptim is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with roboptim.  If not, see <http://www.gnu.org/licenses/>.

#include <boost/shared_ptr.hpp>

#include <coin/IpIpoptApplication.hpp>
#include <coin/IpTNLP.hpp>

#include <roboptim/core/plugin/ipopt/ipopt-td.hh>
#include <roboptim/core/plugin/ipopt/ipopt-sparse.hh>

#include <roboptim/core/debug.hh>
#include <roboptim/core/util.hh>

#include "tnlp.hh"

namespace roboptim
{
  using namespace Ipopt;

  namespace detail
  {
    template <>
    bool
    Tnlp<IpoptSolverSparse>::get_nlp_info (Index& n, Index& m, Index& nnz_jac_g,
					   Index& nnz_h_lag,
					   TNLP::IndexStyleEnum& index_style)
    {
      using namespace boost;

      n = static_cast<Index> (solver_.problem ().function ().inputSize ());
      m = static_cast<Index> (constraintsOutputSize ());

      function_t::vector_t x (n);
      if (solver_.problem ().startingPoint ())
        x = *(solver_.problem ().startingPoint ());
      else
	{
	  // FIXME: should make sure we are in the bounds.
	  x.setZero ();
	}

      // compute number of non zeros elements in jacobian constraint.
      nnz_jac_g = 0;
      typedef solver_t::problem_t::constraints_t::const_iterator
	citer_t;
      for (citer_t it = solver_.problem ().constraints ().begin ();
	   it != solver_.problem ().constraints ().end (); ++it)
	{
	  shared_ptr<solver_t::commonConstraintFunction_t> g;
	  if (it->which () == LINEAR)
	    g = get<shared_ptr<linearFunction_t> > (*it);
	  else
	    g = get<shared_ptr<nonLinearFunction_t> > (*it);

	  nnz_jac_g += g->jacobian (x).nonZeros ();
	}


      nnz_h_lag = 0; // unused
      index_style = TNLP::C_STYLE;
      return true;
    }

    template <>
    bool
    Tnlp<IpoptSolverSparse>::eval_jac_g(Index n, const Number* x, bool,
					Index ROBOPTIM_DEBUG_ONLY(m),
					Index nele_jac,
					Index* iRow, Index *jCol,
					Number* values)
    {
      using namespace boost;
      ROBOPTIM_DEBUG_ONLY
	(function_t::size_type n_ = static_cast<function_t::size_type> (n));
      assert (solver_.problem ().function ().inputSize () == n_);
      assert (constraintsOutputSize () == m);

      if (!jacobian_)
	{
	  jacobian_ = function_t::jacobian_t
	    (static_cast<function_t::matrix_t::Index> (constraintsOutputSize ()),
	     solver_.problem ().function ().inputSize ());
	  jacobian_->reserve (nele_jac);
	}

      if (!values)
	{
	  LOG4CXX_TRACE
	    (logger, "Looking for non-zeros elements.");
	  LOG4CXX_TRACE (logger, "nele_jac = " << nele_jac);

	  // Emptying iRow/jCol arrays.
	  memset (iRow, 0, static_cast<std::size_t> (nele_jac) * sizeof (Index));
	  memset (jCol, 0, static_cast<std::size_t> (nele_jac) * sizeof (Index));

	  // First evaluate the constraints in zero to build the
	  // constraints jacobian.
	  int idx = 0;
	  typedef solver_t::problem_t::constraints_t::const_iterator
	    citer_t;
	  unsigned constraintId = 0;

	  typedef Eigen::Triplet<double> triplet_t;
	  std::vector<triplet_t> coefficients;

	  for (citer_t it = solver_.problem ().constraints ().begin ();
	       it != solver_.problem ().constraints ().end ();
	       ++it, ++constraintId)
	    {
	      LOG4CXX_TRACE
		(logger,
		 "Compute jacobian of constraint id = " << constraintId
		 << "to count for non-zeros elements");

	      function_t::vector_t x (n);
	      // Look for a place to evaluate the jacobian of the
	      // current constraint.
	      // If we do not have an initial guess...
	      if (!solver_.problem ().startingPoint ())
                for (function_t::vector_t::Index i = 0; i < x.size (); ++i)
                  {
                    std::size_t ii = static_cast<std::size_t> (i);

		    // if constraint is in an interval, evaluate at middle.
                    if (solver_.problem ().boundsVector ()[constraintId][ii].first
			!= Function::infinity ()
			&&
                        solver_.problem ().boundsVector ()[constraintId][ii].second
			!= Function::infinity ())
                      x[i] =
			(solver_.problem ().boundsVector ()
                         [constraintId][ii].second
			 - solver_.problem ().boundsVector ()
                         [constraintId][ii].first) / 2.;
		    // otherwise use the non-infinite bound.
		    else if (solver_.problem ().boundsVector ()
                             [constraintId][ii].first
			     != Function::infinity ())
		      x[i] = solver_.problem ().boundsVector ()
                        [constraintId][ii].first;
		    else
		      x[i] = solver_.problem ().boundsVector ()
                        [constraintId][ii].second;
		  }
	      else // other use initial guess.
		x = *(solver_.problem ().startingPoint ());

	      shared_ptr<solver_t::commonConstraintFunction_t> g;
	      if (it->which () == LINEAR)
		g = get<shared_ptr<linearFunction_t> > (*it);
	      else
		g = get<shared_ptr<nonLinearFunction_t> > (*it);

	      constraintJacobians_.push_back (g->jacobian (x));
	      function_t::jacobian_t& tmp_jac = constraintJacobians_.back ();
	      tmp_jac.makeCompressed ();
	      for (int k = 0; k < tmp_jac.outerSize (); ++k)
		for (function_t::jacobian_t::InnerIterator
		       it (tmp_jac, k); it; ++it)
		  {
                    const int row = static_cast<int> (idx + it.row ());
                    const int col = static_cast<int> (it.col ());
		    coefficients.push_back
		      (triplet_t (row, col, it.value ()));
		  }
	      idx += g->outputSize ();
	    }

	  jacobian_->setFromTriplets
	    (coefficients.begin (), coefficients.end ());

	  LOG4CXX_TRACE
	    (logger, "full problem jacobian...\n" << *jacobian_);

	  // Then look for non-zero values.
	  LOG4CXX_TRACE (logger, "filling iRow and jCol...");
	  idx = 0;

	  for (int k = 0; k < jacobian_->outerSize (); ++k)
	    for (function_t::jacobian_t::InnerIterator it (*jacobian_, k);
		 it; ++it)
	      {
		iRow[idx] = it.row (), jCol[idx] = it.col ();
		LOG4CXX_TRACE
		  (logger, "row: " << it.row ()
		   << " / col: " << it.col ()
		   << " / index: " << it.index ()
		   << " / value: " << it.value ()
		   << "\nidx: " << idx);
		++idx;
	      }

	  return true;
	}

      Eigen::Map<const function_t::vector_t> x_ (x, n);

      typedef solver_t::problem_t::constraints_t::const_iterator
	citer_t;

      size_t constraintId = 0;
      for (citer_t it = solver_.problem ().constraints ().begin ();
	   it != solver_.problem ().constraints ().end (); ++it, constraintId++)
	{
	  shared_ptr<solver_t::commonConstraintFunction_t> g;
	  if (it->which () == LINEAR)
	    g = get<shared_ptr<linearFunction_t> > (*it);
	  else
	    g = get<shared_ptr<nonLinearFunction_t> > (*it);

	  typename function_t::matrix_t& jac = constraintJacobians_[constraintId];
	  // Set the Jacobian to 0 while keeping its structure
	  jac *= 0.;
	  g->jacobian (jac, x_);

	  IpoptCheckGradient
	    (*g, 0, x_, static_cast<int> (constraintId), solver_);
	}

      // Copy jacobian values from internal sparse matrices.
      int idx = 0;

      if (StorageOrder == Eigen::ColMajor)
	{
	  for (int k = 0; k < solver_.problem ().function ().inputSize (); ++k)
	    for (constraintJacobians_t::const_iterator
		   g  = constraintJacobians_.begin ();
		 g != constraintJacobians_.end (); ++g)
	      {
		for (function_t::jacobian_t::InnerIterator it (*g, k);
		     it; ++it)
		  {
		    assert (idx < nele_jac);
		    values[idx++] = it.value ();
		  }
	      }
	}
      else
	{
	  for (constraintJacobians_t::const_iterator
		 g  = constraintJacobians_.begin ();
	       g != constraintJacobians_.end (); ++g)
	    for (int k = 0; k < g->outerSize(); ++k)
	      for (function_t::jacobian_t::InnerIterator it (*g, k);
		   it; ++it)
		{
		  assert (idx < nele_jac);
		  values[idx++] = it.value ();
		}
	}

      return true;
    }
  } // end of namespace detail
} // end of namespace roboptim
