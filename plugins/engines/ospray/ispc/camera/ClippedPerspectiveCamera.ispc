// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "ClippedPerspectiveCamera.ih"
#include "math/sampling.ih"

const float EPSILON = 0.0000001f;

void getClippingValues(uniform ClippedPerspectiveCamera* uniform self,
                       const varying vec3f& worldEyePosition,
                       const varying vec3f& rayDirection, varying float& near,
                       varying float& far)
{
    for (int i = 0; i < NB_CLIP_PLANES; ++i)
    {
        const vec3f planeNormal = {self->clipPlanes[i].x, self->clipPlanes[i].y,
                                   self->clipPlanes[i].z};
        float rn = dot(rayDirection, planeNormal);
        if (rn == 0.f)
            rn = EPSILON;
        float d = self->clipPlanes[i].w;
        float t = -(dot(planeNormal, worldEyePosition) + d) / rn;
        if (rn > 0.f) // opposite direction plane
            near = max(near, t);
        else
            far = min(far, t);
    }
}

void ClippedPerspectiveCamera_initRay(uniform Camera* uniform _self,
                                      varying Ray& ray,
                                      const varying CameraSample& sample)
{
    uniform ClippedPerspectiveCamera* uniform self =
        (uniform ClippedPerspectiveCamera * uniform)_self;

    vec2f screen = sample.screen;
    vec3f org = self->org;

    if (self->side_by_side)
    {
        screen.x *= 2.f;
        if (screen.x < 1.f)
            org = org - self->ipd_offset;
        else
        {
            org = org + self->ipd_offset;
            screen.x -= 1.f;
        }
    }

    screen = Camera_subRegion(_self, screen);

    vec3f dir =
        self->dir_00 + screen.x * self->dir_du + screen.y * self->dir_dv;

    if (self->super.doesDOF)
    {
        const vec3f llp = uniformSampleDisk(self->scaledAperture, sample.lens);
        // transform local lens point to focal plane (dir_XX are prescaled in
        // this case)
        const vec3f lp =
            (llp.x * self->dir_du) + ((llp.y * self->aspect) * self->dir_dv);
        org = org + lp;
        dir = dir - lp;
    }

    // Define clipping min and max
    float nearClip = self->super.nearClip;
    float farClip = infinity;
    const varying vec3f direction = normalize(dir);
    getClippingValues(self, org, direction, nearClip, farClip);

    // Sets clipped ray
    setRay(ray, org, direction, nearClip, farClip);
}

/*! create a new ispc-side version of a ClippedPerspectiveCamera - with given
    pointer to the c-class version - and return both class pointer and
    pointer to internal data back via the supplied reference
    parameters */
export void* uniform ClippedPerspectiveCamera_create(void* uniform cppE)
{
    uniform ClippedPerspectiveCamera* uniform self =
        uniform new uniform ClippedPerspectiveCamera;
    self->super.cppEquivalent = cppE;
    self->super.initRay = ClippedPerspectiveCamera_initRay;
    self->super.doesDOF = false;
    return self;
}

export void ClippedPerspectiveCamera_set(
    void* uniform _self, const uniform vec3f& org, const uniform vec3f& dir_00,
    const uniform vec3f& dir_du, const uniform vec3f& dir_dv,
    const uniform float scaledAperture, const uniform float aspect,
    const uniform bool side_by_side, const uniform vec3f& ipd_offset,
    const uniform vec4f& clipPlane1, const uniform vec4f& clipPlane2,
    const uniform vec4f& clipPlane3, const uniform vec4f& clipPlane4,
    const uniform vec4f& clipPlane5, const uniform vec4f& clipPlane6)
{
    uniform ClippedPerspectiveCamera* uniform self =
        (uniform ClippedPerspectiveCamera * uniform)_self;
    self->org = org;
    self->dir_00 = dir_00;
    self->dir_du = dir_du;
    self->dir_dv = dir_dv;
    self->scaledAperture = scaledAperture;
    self->super.doesDOF = scaledAperture > 0.f;
    self->aspect = aspect;
    self->side_by_side = side_by_side;
    self->ipd_offset = ipd_offset;
    self->clipPlanes[0] = clipPlane1;
    self->clipPlanes[1] = clipPlane2;
    self->clipPlanes[2] = clipPlane3;
    self->clipPlanes[3] = clipPlane4;
    self->clipPlanes[4] = clipPlane5;
    self->clipPlanes[5] = clipPlane6;
}
