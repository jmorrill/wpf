// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

using MS.Internal;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows.Media.Composition;

namespace System.Windows.Media.Effects
{
    /// <summary>
    /// Property types for D2D effect properties.
    /// Maps to D2D1_PROPERTY_TYPE values used by ID2D1Effect::SetValue.
    /// </summary>
    public enum D2DEffectPropertyType : uint
    {
        Float = 0,
        Float2 = 1,
        Float3 = 2,
        Float4 = 3,
        Int = 4,
        Bool = 5,
        Matrix3x2 = 6,
        Matrix4x4 = 7,
        Matrix5x4 = 8,
        Enum = 9,
    }

    /// <summary>
    /// Input kind for D2D effect inputs - either a brush or another effect.
    /// </summary>
    internal enum D2DEffectInputKind : uint
    {
        Brush = 0,
        Effect = 1,
    }

    /// <summary>
    /// Base class for WPF effects backed by Direct2D built-in effects.
    /// Supports arbitrary D2D effect CLSIDs, a property bag, and multi-input
    /// (including effect chaining via SetInput with another D2DEffect, and
    /// brush-based inputs via SetInput with a Brush such as VisualBrush).
    /// 
    /// When the D2D engine is not active, the effect is a no-op and the
    /// element renders normally.
    /// </summary>
    public class D2DEffect : Effect
    {
        #region Constructors

        /// <summary>
        /// Creates a D2DEffect with the specified D2D built-in effect CLSID.
        /// </summary>
        public D2DEffect(Guid effectId)
        {
            _effectId = effectId;
        }

        /// <summary>
        /// Protected constructor for typed subclasses that set EffectId themselves.
        /// </summary>
        protected D2DEffect()
        {
        }

        #endregion

        #region Public Properties

        /// <summary>
        /// The D2D built-in effect CLSID.
        /// </summary>
        public Guid EffectId
        {
            get { return _effectId; }
            protected set
            {
                WritePreamble();
                _effectId = value;
                RegisterForAsyncUpdateResource();
                WritePostscript();
            }
        }

        #endregion

        #region Input Management

        /// <summary>
        /// Sets a Brush as the input at the specified index.
        /// Use Effect.ImplicitInput for the element's own rendered content.
        /// Use VisualBrush to blend another visual's content.
        /// </summary>
        public void SetInput(int index, Brush brush)
        {
            ArgumentOutOfRangeException.ThrowIfNegative(index);
            WritePreamble();

            if (brush == null)
            {
                _inputs.Remove(index);
            }
            else
            {
                _inputs[index] = new D2DEffectInputEntry
                {
                    Kind = D2DEffectInputKind.Brush,
                    BrushInput = brush,
                    EffectInput = null,
                };
            }

            RegisterForAsyncUpdateResource();
            WritePostscript();
        }

        /// <summary>
        /// Sets another D2DEffect as the input at the specified index (effect chaining).
        /// </summary>
        public void SetInput(int index, D2DEffect effect)
        {
            ArgumentOutOfRangeException.ThrowIfNegative(index);
            WritePreamble();

            if (effect == null)
            {
                _inputs.Remove(index);
            }
            else
            {
                _inputs[index] = new D2DEffectInputEntry
                {
                    Kind = D2DEffectInputKind.Effect,
                    BrushInput = null,
                    EffectInput = effect,
                };
            }

            RegisterForAsyncUpdateResource();
            WritePostscript();
        }

        /// <summary>
        /// Clears the input at the specified index.
        /// </summary>
        public void ClearInput(int index)
        {
            WritePreamble();
            _inputs.Remove(index);
            RegisterForAsyncUpdateResource();
            WritePostscript();
        }

        #endregion

        #region Property Bag

        /// <summary>
        /// Sets a property value by D2D property index.
        /// </summary>
        protected void SetValue(int index, float value)
        {
            SetPropertyRaw(index, D2DEffectPropertyType.Float, BitConverter.GetBytes(value));
        }

        /// <summary>
        /// Sets a property value by D2D property index (two floats).
        /// </summary>
        protected void SetValue(int index, float x, float y)
        {
            byte[] data = new byte[8];
            BitConverter.GetBytes(x).CopyTo(data, 0);
            BitConverter.GetBytes(y).CopyTo(data, 4);
            SetPropertyRaw(index, D2DEffectPropertyType.Float2, data);
        }

        /// <summary>
        /// Sets a Vector4 property (four floats).
        /// </summary>
        protected void SetValue(int index, float x, float y, float z, float w)
        {
            byte[] data = new byte[16];
            BitConverter.GetBytes(x).CopyTo(data, 0);
            BitConverter.GetBytes(y).CopyTo(data, 4);
            BitConverter.GetBytes(z).CopyTo(data, 8);
            BitConverter.GetBytes(w).CopyTo(data, 12);
            SetPropertyRaw(index, D2DEffectPropertyType.Float4, data);
        }

        /// <summary>
        /// Sets an integer/enum property.
        /// </summary>
        protected void SetValue(int index, int value)
        {
            SetPropertyRaw(index, D2DEffectPropertyType.Enum, BitConverter.GetBytes(value));
        }

        /// <summary>
        /// Sets a bool property.
        /// </summary>
        protected void SetValue(int index, bool value)
        {
            SetPropertyRaw(index, D2DEffectPropertyType.Bool, BitConverter.GetBytes(value ? 1 : 0));
        }

        /// <summary>
        /// Sets a Matrix5x4 property (20 floats = 80 bytes).
        /// </summary>
        protected void SetMatrix5x4(int index, float[] matrix)
        {
            if (matrix == null || matrix.Length != 20)
                throw new ArgumentException("Matrix5x4 requires exactly 20 float values.", nameof(matrix));

            byte[] data = new byte[80];
            for (int i = 0; i < 20; i++)
            {
                BitConverter.GetBytes(matrix[i]).CopyTo(data, i * 4);
            }
            SetPropertyRaw(index, D2DEffectPropertyType.Matrix5x4, data);
        }

        /// <summary>
        /// Sets a raw property value. This is the most flexible overload.
        /// </summary>
        public void SetPropertyRaw(int index, D2DEffectPropertyType type, byte[] value)
        {
            ArgumentOutOfRangeException.ThrowIfNegative(index);
            ArgumentNullException.ThrowIfNull(value);
            WritePreamble();

            _properties[index] = new D2DEffectPropertyEntry
            {
                Index = index,
                Type = type,
                SerializedValue = value,
            };

            RegisterForAsyncUpdateResource();
            WritePostscript();
        }

        #endregion

        #region Effect Overrides

        internal override Rect GetRenderBounds(Rect contentBounds)
        {
            double pad = EffectPadding;
            if (pad > 0)
            {
                return new Rect(
                    contentBounds.X - pad,
                    contentBounds.Y - pad,
                    contentBounds.Width + pad * 2,
                    contentBounds.Height + pad * 2);
            }
            return contentBounds;
        }

        /// <summary>
        /// Padding in DIPs to expand the render bounds beyond the element bounds.
        /// Subclasses should set this to ensure effects like blur and shadow
        /// have room to render outside the original element area.
        /// </summary>
        protected double EffectPadding
        {
            get => _effectPadding;
            set
            {
                WritePreamble();
                _effectPadding = value;
                RegisterForAsyncUpdateResource();
                WritePostscript();
            }
        }

        protected override Freezable CreateInstanceCore()
        {
            return new D2DEffect(_effectId);
        }

        #endregion

        #region DUCE Resource Management

        internal override void UpdateResource(DUCE.Channel channel, bool skipOnChannelCheck)
        {
            Debug.Assert(!skipOnChannelCheck || _duceResource.IsOnChannel(channel));

            if (skipOnChannelCheck || _duceResource.IsOnChannel(channel))
            {
                base.UpdateResource(channel, skipOnChannelCheck);

                // Ensure input 0 defaults to ImplicitInput if not set
                bool hasInput0 = _inputs.ContainsKey(0);

                // ── Pre-resolve all resource handles BEFORE BeginCommand ──
                // AddRefOnChannel can send its own DUCE commands, so it must
                // happen outside the BeginCommand/EndCommand bracket.
                DUCE.ResourceHandle hImplicit = DUCE.ResourceHandle.Null;
                if (!hasInput0)
                {
                    hImplicit = ((DUCE.IResource)ImplicitInput).AddRefOnChannel(channel);
                }

                // Build resolved input list
                var resolvedInputs = new List<(uint Index, uint Kind, uint Handle)>();
                if (!hasInput0)
                {
                    resolvedInputs.Add((0, (uint)D2DEffectInputKind.Brush, (uint)hImplicit));
                }
                foreach (var kvp in _inputs)
                {
                    uint inputIndex = (uint)kvp.Key;
                    var entry = kvp.Value;
                    uint inputKind = (uint)entry.Kind;
                    DUCE.ResourceHandle hResource;

                    if (entry.Kind == D2DEffectInputKind.Effect && entry.EffectInput != null)
                    {
                        hResource = ((DUCE.IResource)entry.EffectInput).AddRefOnChannel(channel);
                    }
                    else if (entry.Kind == D2DEffectInputKind.Brush && entry.BrushInput != null)
                    {
                        hResource = ((DUCE.IResource)entry.BrushInput).AddRefOnChannel(channel);
                    }
                    else
                    {
                        hResource = DUCE.ResourceHandle.Null;
                    }

                    resolvedInputs.Add((inputIndex, inputKind, (uint)hResource));
                }

                // Serialize properties
                int propertyDataSize = 0;
                foreach (var kvp in _properties)
                {
                    // Each property entry: uint index (4) + uint type (4) + uint dataSize (4) + data
                    propertyDataSize += 12 + kvp.Value.SerializedValue.Length;
                }

                int inputCount = resolvedInputs.Count;
                // Each input entry: uint inputIndex (4) + uint inputKind (4) + HMIL_RESOURCE handle (4) = 12 bytes
                int inputDataSize = inputCount * 12;

                int cbExtra = propertyDataSize + inputDataSize;

                unsafe
                {
                    DUCE.MILCMD_D2DEFFECT data;
                    data.Type = MILCMD.MilCmdD2DEffect;
                    data.Handle = _duceResource.GetHandle(channel);
                    data.EffectClsid = _effectId;
                    data.PropertyCount = (uint)_properties.Count;
                    data.PropertyDataSize = (uint)propertyDataSize;
                    data.InputCount = (uint)inputCount;
                    data.InputDataSize = (uint)inputDataSize;
                    data.Padding = (float)_effectPadding;

                    channel.BeginCommand(
                        (byte*)&data,
                        sizeof(DUCE.MILCMD_D2DEFFECT),
                        cbExtra);

                    // Append property data
                    foreach (var kvp in _properties)
                    {
                        var entry = kvp.Value;
                        uint idx = (uint)entry.Index;
                        uint type = (uint)entry.Type;
                        uint dataLen = (uint)entry.SerializedValue.Length;

                        channel.AppendCommandData((byte*)&idx, 4);
                        channel.AppendCommandData((byte*)&type, 4);
                        channel.AppendCommandData((byte*)&dataLen, 4);

                        fixed (byte* pData = entry.SerializedValue)
                        {
                            channel.AppendCommandData(pData, (int)dataLen);
                        }
                    }

                    // Append pre-resolved input handles
                    foreach (var (inputIdx, inputKind, hVal) in resolvedInputs)
                    {
                        uint idx = inputIdx;
                        uint kind = inputKind;
                        uint handle = hVal;

                        channel.AppendCommandData((byte*)&idx, 4);
                        channel.AppendCommandData((byte*)&kind, 4);
                        channel.AppendCommandData((byte*)&handle, 4);
                    }

                    channel.EndCommand();
                }
            }
        }

        internal override DUCE.ResourceHandle AddRefOnChannelCore(DUCE.Channel channel)
        {
            if (_duceResource.CreateOrAddRefOnChannel(this, channel, DUCE.ResourceType.TYPE_D2DEFFECT))
            {
                UpdateResource(channel, true);
            }

            return _duceResource.GetHandle(channel);
        }

        internal override void ReleaseOnChannelCore(DUCE.Channel channel)
        {
            Debug.Assert(_duceResource.IsOnChannel(channel));

            if (_duceResource.ReleaseOnChannel(channel))
            {
                // Release input resources on the channel
                foreach (var kvp in _inputs)
                {
                    var entry = kvp.Value;
                    if (entry.Kind == D2DEffectInputKind.Brush && entry.BrushInput != null)
                    {
                        ((DUCE.IResource)entry.BrushInput).ReleaseOnChannel(channel);
                    }
                    else if (entry.Kind == D2DEffectInputKind.Effect && entry.EffectInput != null)
                    {
                        ((DUCE.IResource)entry.EffectInput).ReleaseOnChannel(channel);
                    }
                }
            }
        }

        internal override DUCE.ResourceHandle GetHandleCore(DUCE.Channel channel)
        {
            return _duceResource.GetHandle(channel);
        }

        internal override int GetChannelCountCore()
        {
            return _duceResource.GetChannelCount();
        }

        internal override DUCE.Channel GetChannelCore(int index)
        {
            return _duceResource.GetChannel(index);
        }

        #endregion

        #region Internal Types

        internal struct D2DEffectPropertyEntry
        {
            public int Index;
            public D2DEffectPropertyType Type;
            public byte[] SerializedValue;
        }

        internal struct D2DEffectInputEntry
        {
            public D2DEffectInputKind Kind;
            public Brush BrushInput;
            public D2DEffect EffectInput;
        }

        #endregion

        #region Fields

        private Guid _effectId;
        private readonly Dictionary<int, D2DEffectPropertyEntry> _properties = new Dictionary<int, D2DEffectPropertyEntry>();
        private readonly Dictionary<int, D2DEffectInputEntry> _inputs = new Dictionary<int, D2DEffectInputEntry>();
        private double _effectPadding;
        internal System.Windows.Media.Composition.DUCE.MultiChannelResource _duceResource = new DUCE.MultiChannelResource();

        #endregion
    }
}
