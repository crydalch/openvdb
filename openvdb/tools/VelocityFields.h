///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2012-2014 DreamWorks Animation LLC
//
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
//
// Redistributions of source code must retain the above copyright
// and license notice and the following restrictions and disclaimer.
//
// *     Neither the name of DreamWorks Animation nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// IN NO EVENT SHALL THE COPYRIGHT HOLDERS' AND CONTRIBUTORS' AGGREGATE
// LIABILITY FOR ALL CLAIMS REGARDLESS OF THEIR BASIS EXCEED US$250.00.
//
///////////////////////////////////////////////////////////////////////////
//
///////////////////////////////////////////////////////////////////////////
//
/// @author Ken Museth
///
/// @file VelocityFields.h
///
/// @brief Defines two simple wrapper classes for advection velocity
///        fields as well as VelocitySampler and VelocityIntegrator
///
///     
/// @details DiscreteField wraps a velocity grid and EnrightField is mostly
///          intended for debugging (it's an analytical divergence free and
///          periodic field). They both share the same API required by the
///          LevelSetAdvection class defined in LevelSetAdvect.h. Thus, any
///          class with this API should work with LevelSetAdvection.
///
/// @warning Note the Field wrapper classes below always assume the velocity
///          is represented in the world-frame of reference. For DiscreteField
///          this implies the input grid must contain velocities in world
///          coordinates.

#ifndef OPENVDB_TOOLS_VELOCITY_FIELDS_HAS_BEEN_INCLUDED
#define OPENVDB_TOOLS_VELOCITY_FIELDS_HAS_BEEN_INCLUDED

#include <tbb/parallel_reduce.h>
#include <openvdb/Platform.h>
#include "Interpolation.h" // for Sampler, etc.
#include <openvdb/math/FiniteDifference.h>
#include <boost/math/constants/constants.hpp>

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {
namespace tools {

/// @brief Thin wrapper class for a velocity grid
/// @note Consider replacing BoxSampler with StaggeredBoxSampler
template <typename VelGridT, typename Interpolator = BoxSampler>
class DiscreteField
{
public:
    typedef typename VelGridT::ValueType     VectorType;
    typedef typename VectorType::ValueType   ValueType;
    BOOST_STATIC_ASSERT(boost::is_floating_point<ValueType>::value);

    DiscreteField(const VelGridT &vel): mAccessor(vel.tree()), mTransform(&vel.transform())
    {
    }

    /// @return const reference to the transfrom between world and index space
    /// @note Use this method to determine if a client grid is
    /// aligned with the coordinate space of the velocity grid.
    const math::Transform& transform() const { return *mTransform; }

    /// @return the interpolated velocity at the world space position xyz
    inline VectorType operator() (const Vec3d& xyz, ValueType) const
    {
        return Interpolator::sample(mAccessor, mTransform->worldToIndex(xyz));
    }

    /// @return the velocity at the coordinate space position ijk
    inline VectorType operator() (const Coord& ijk, ValueType) const
    {
        return mAccessor.getValue(ijk);
    }

private:
    const typename VelGridT::ConstAccessor mAccessor;//Not thread-safe
    const math::Transform*                 mTransform;

}; // end of DiscreteField

///////////////////////////////////////////////////////////////////////
    
/// @brief Analytical, divergence-free and periodic vecloity field
/// @note Primarily intended for debugging!
/// @warning This analytical velocity only produce meaningfull values
/// in the unitbox in world space. In other words make sure any level
/// set surface in fully enclodes in the axis aligned bounding box
/// spanning 0->1 in world units.
template <typename ScalarT = float>
class EnrightField
{
public:
    typedef ScalarT             ValueType;
    typedef math::Vec3<ScalarT> VectorType;
    BOOST_STATIC_ASSERT(boost::is_floating_point<ScalarT>::value);

    EnrightField() {}

    /// @return const reference to the identity transfrom between world and index space
    /// @note Use this method to determine if a client grid is
    /// aligned with the coordinate space of this velocity field
    math::Transform transform() const { return math::Transform(); }

    /// @return the velocity in world units, evaluated at the world
    /// position xyz and at the specified time
    inline VectorType operator() (const Vec3d& xyz, ValueType time) const;

    /// @return the velocity at the coordinate space position ijk
    inline VectorType operator() (const Coord& ijk, ValueType time) const
    {
        return (*this)(ijk.asVec3d(), time);
    }
}; // end of EnrightField

template <typename ScalarT>
inline math::Vec3<ScalarT>
EnrightField<ScalarT>::operator() (const Vec3d& xyz, ValueType time) const
{
    const ScalarT pi = boost::math::constants::pi<ScalarT>();
    const ScalarT phase = pi / ScalarT(3.0);
    const ScalarT Px =  pi * ScalarT(xyz[0]), Py = pi * ScalarT(xyz[1]), Pz = pi * ScalarT(xyz[2]);
    const ScalarT tr =  cos(ScalarT(time) * phase);
    const ScalarT a  =  sin(ScalarT(2.0)*Py);
    const ScalarT b  = -sin(ScalarT(2.0)*Px);
    const ScalarT c  =  sin(ScalarT(2.0)*Pz);
    return math::Vec3<ScalarT>(
        tr * ( ScalarT(2) * math::Pow2(sin(Px)) * a * c ),
        tr * ( b * math::Pow2(sin(Py)) * c ),
        tr * ( b * a * math::Pow2(sin(Pz)) ));
}


///////////////////////////////////////////////////////////////////////

/// Class to hold a Vec3 field interperated as a velocity field.
/// Primarily exists to provide a method(s) that integrate a passive
/// point forward in the velocity field for a single time-step (dt)
template<typename GridT = Vec3fGrid,
         bool Staggered = false,
         size_t Order = 1>
class VelocitySampler
{
public:
    typedef typename GridT::ConstAccessor AccessorType;
    typedef typename GridT::ValueType     ValueType;

    VelocitySampler(const GridT& grid):
        mGrid(&grid),
        mAcc(grid.getAccessor())
    {
    }
    VelocitySampler(const VelocitySampler& other):
        mGrid(other.mGrid),
        mAcc(mGrid->getAccessor())
    {
    }
    /// Samples the velocity at position W onto result. Supports both
    /// staggered (i.e. MAC) and collocated velocity grids.
    /// @return @c true if any one of the sampled values is active.
    template <typename LocationType>
    inline bool sample(const LocationType& W, ValueType& result) const
    {
        const Vec3R xyz = mGrid->worldToIndex(Vec3R(W[0], W[1], W[2]));
        bool active = Sampler<Order, Staggered>::sample(mAcc, xyz, result);
        return active;
    }

    /// Samples the velocity at position W onto result. Supports both
    /// staggered (i.e. MAC) and collocated velocity grids.
    template <typename LocationType>
    inline ValueType sample(const LocationType& W) const
    {
        const Vec3R xyz = mGrid->worldToIndex(Vec3R(W[0], W[1], W[2]));
        return Sampler<Order, Staggered>::sample(mAcc, xyz);
    }

private:
    // holding the Grids for the transforms
    const GridT* mGrid; // Velocity vector field
    AccessorType mAcc;
};// end of VelocitySampler class

///////////////////////////////////////////////////////////////////////

/// @brief Performs runge-kutta time integration of variable order in
/// a static velocity field
template<typename GridT = Vec3fGrid,
         bool Staggered = false>
class VelocityIntegrator
{
public:
    typedef typename GridT::ValueType  VecType;
    typedef typename VecType::ValueType ElementType;

    VelocityIntegrator(const GridT& velGrid):
        mVelSampler(velGrid)
    {
    }
    /// @brief Variable order Runge-Kutta time integration for a single time step
    /// @note  Note the order of the velocity sampling is always one,
    ///        i.e. based on tri-linear interpolation!
    template<size_t Order, typename LocationType>
    inline void rungeKutta(const float dt, LocationType& loc) const
    {
        BOOST_STATIC_ASSERT(Order < 5);
        VecType P(loc[0],loc[1],loc[2]);
        // Note the if-braching below is optimized away at compile time
        if (Order == 0) {
            return;// do nothing
        } else if (Order == 1) {
            VecType V0;
            mVelSampler.sample(P, V0);
            P =  dt*V0;
        } else if (Order == 2) {
            VecType V0, V1;
            mVelSampler.sample(P, V0);
            mVelSampler.sample(P + ElementType(0.5) * ElementType(dt) * V0, V1);
            P = dt*V1;
        } else if (Order == 3) {
            VecType V0, V1, V2;
            mVelSampler.sample(P, V0);
            mVelSampler.sample(P+ElementType(0.5)*ElementType(dt)*V0, V1);
            mVelSampler.sample(P+dt*(ElementType(2.0)*V1-V0), V2);
            P = dt*(V0 + ElementType(4.0)*V1 + V2)*ElementType(1.0/6.0);
        } else if (Order == 4) {
            VecType V0, V1, V2, V3;
            mVelSampler.sample(P, V0);
            mVelSampler.sample(P+ElementType(0.5)*ElementType(dt)*V0, V1);
            mVelSampler.sample(P+ElementType(0.5)*ElementType(dt)*V1, V2);
            mVelSampler.sample(P+     dt*V2, V3);
            P = dt*(V0 + ElementType(2.0)*(V1 + V2) + V3)*ElementType(1.0/6.0);
        }
        loc += LocationType(P[0], P[1], P[2]);

    }
private:
    VelocitySampler<GridT, Staggered, /*order=*/1> mVelSampler;
};// end of VelocityIntegrator class


} // namespace tools
} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

#endif // OPENVDB_TOOLS_VELOCITY_FIELDS_HAS_BEEN_INCLUDED

// Copyright (c) 2012-2014 DreamWorks Animation LLC
// All rights reserved. This software is distributed under the
// Mozilla Public License 2.0 ( http://www.mozilla.org/MPL/2.0/ )
