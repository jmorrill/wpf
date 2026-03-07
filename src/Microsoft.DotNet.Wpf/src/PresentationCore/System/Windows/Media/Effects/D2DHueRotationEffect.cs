// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Hue Rotation effect. Rotates the hue of the input.
    /// </summary>
    public class D2DHueRotationEffect : D2DEffect
    {
        private const int D2D1_HUEROTATION_PROP_ANGLE = 0;

        public D2DHueRotationEffect()
        {
            EffectId = D2DEffects.HueRotation;
            Angle = 0.0f;
        }

        /// <summary>
        /// The hue rotation angle in degrees. Default is 0.
        /// </summary>
        public float Angle
        {
            get => _angle;
            set
            {
                _angle = value;
                SetValue(D2D1_HUEROTATION_PROP_ANGLE, value);
            }
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DHueRotationEffect();
        }

        private float _angle;
    }
}
