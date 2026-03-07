// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// D2D Color Matrix effect. Applies a 5x4 color transformation matrix.
    /// </summary>
    public class D2DColorMatrixEffect : D2DEffect
    {
        private const int D2D1_COLORMATRIX_PROP_COLOR_MATRIX = 0;
        private const int D2D1_COLORMATRIX_PROP_ALPHA_MODE = 1;
        private const int D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT = 2;

        public D2DColorMatrixEffect()
        {
            EffectId = D2DEffects.ColorMatrix;
        }

        /// <summary>
        /// Sets the 5x4 color matrix. Must be exactly 20 float values in row-major order.
        /// </summary>
        public void SetColorMatrix(float[] matrix)
        {
            if (matrix == null || matrix.Length != 20)
                throw new ArgumentException("Matrix must contain exactly 20 float values.", nameof(matrix));
            
            _colorMatrix = (float[])matrix.Clone();
            SetMatrix5x4(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, _colorMatrix);
        }

        /// <summary>
        /// Alpha mode. 0 = Premultiplied (default), 1 = Straight.
        /// </summary>
        public int AlphaMode
        {
            get => _alphaMode;
            set
            {
                _alphaMode = value;
                SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, value);
            }
        }

        /// <summary>
        /// Whether to clamp output to [0,1]. Default is false.
        /// </summary>
        public bool ClampOutput
        {
            get => _clampOutput;
            set
            {
                _clampOutput = value;
                SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, value);
            }
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DColorMatrixEffect();
        }

        private float[] _colorMatrix;
        private int _alphaMode;
        private bool _clampOutput;
    }
}
