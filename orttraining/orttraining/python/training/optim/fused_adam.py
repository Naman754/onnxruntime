# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.
# fused_adam.py
# This file has been adapted from microsoft/DeepSpeed

'''
Copyright 2020 The Microsoft DeepSpeed Team

Copyright NVIDIA/apex
This file is adapted from fused adam in NVIDIA/apex, commit a109f85
'''

import torch
from ._multi_tensor_apply import MultiTensorApply


class FusedAdam(torch.optim.Optimizer):
    """Implements Adam algorithm.

    The algorithmic implementation is mathematically equivalent to Transformers/AdamW
    as defined here: https://github.com/huggingface/transformers/blob/61f64262692ac7dc90e2e0bdeb7e79d9cd607a66/src/transformers/optimization.py#L349-L370

    Currently GPU-only.

    This version of fused Adam implements 2 fusions.

      * Fusion of the Adam update's elementwise operations
      * A multi-tensor apply launch that batches the elementwise updates applied to all the model's parameters into one or a few kernel launches.

    Adam was been proposed in `Adam: A Method for Stochastic Optimization`_.

    Arguments:
        params (iterable): iterable of parameters to optimize or dicts defining
            parameter groups.
        lr (float, optional): learning rate. (default: 1e-3)
        betas (Tuple[float, float], optional): coefficients used for computing
            running averages of gradient and its square. (default: (0.9, 0.999))
        eps (float, optional): term added to the denominator to improve
            numerical stability. (default: 1e-8)
        weight_decay (float, optional): weight decay (L2 penalty) (default: 0)
        amsgrad (boolean, optional): whether to use the AMSGrad variant of this
            algorithm from the paper `On the Convergence of Adam and Beyond`_
            (default: False) NOT SUPPORTED in FusedAdam!
        adam_w_mode (boolean, optional): Apply L2 regularization or weight decay
            True for decoupled weight decay(also known as AdamW) (default: True)
        set_grad_none (bool, optional): whether set grad to None when zero_grad()
            method is called. (default: True)

    .. _Adam - A Method for Stochastic Optimization:
        https://arxiv.org/abs/1412.6980
    .. _On the Convergence of Adam and Beyond:
        https://openreview.net/forum?id=ryQu7f-RZ
    """
    def __init__(self,
                 params,
                 lr=1e-3,
                 bias_correction=True,
                 betas=(0.9,
                        0.999),
                 eps=1e-6,
                 adam_w_mode=True,
                 weight_decay=0.,
                 amsgrad=False,
                 set_grad_none=False):

        # The FusedAdam implementation is mathematically equivalent to
        # transformers AdamW. The input arguments also have the same defaults.

        if amsgrad:
            raise RuntimeError('FusedAdam does not support the AMSGrad variant.')
        defaults = dict(lr=lr,
                        bias_correction=bias_correction,
                        betas=betas,
                        eps=eps,
                        weight_decay=weight_decay)
        super(FusedAdam, self).__init__(params, defaults)
        self._adam_w_mode = 1 if adam_w_mode else 0
        self._set_grad_none = set_grad_none

        # Skip buffer
        self._dummy_overflow_buf = torch.cuda.IntTensor([0])

        from onnxruntime.training.ortmodule.torch_cpp_extensions import adam_optimizer
        self._multi_tensor_adam = adam_optimizer.multi_tensor_adam
        self._multi_tensor_applier = MultiTensorApply(2048 * 32)

    def zero_grad(self):
        if self._set_grad_none:
            for group in self.param_groups:
                for p in group['params']:
                    p.grad = None
        else:
            super(FusedAdam, self).zero_grad()

    def step(self, closure=None):
        """Performs a single optimization step.

        Arguments:
            closure (callable, optional): A closure that reevaluates the model
                and returns the loss.

        The remaining arguments are deprecated, and are only retained (for the moment) for error-checking purposes.
        """
        loss = None
        if closure is not None:
            loss = closure()

        for group in self.param_groups:
            bias_correction = 1 if group['bias_correction'] else 0
            beta1, beta2 = group['betas']

            # assume same step across group now to simplify things
            # per parameter step can be easily support by making it tensor, or pass list into kernel
            if 'step' in group:
                group['step'] += 1
            else:
                group['step'] = 1

            # create lists for multi-tensor apply
            g_16, p_16, m_16, v_16 = [], [], [], []
            g_32, p_32, m_32, v_32 = [], [], [], []

            for p in group['params']:
                if p.grad is None:
                    continue
                if p.grad.data.is_sparse:
                    raise RuntimeError(
                        'FusedAdam does not support sparse gradients, please consider SparseAdam instead'
                    )

                state = self.state[p]
                # State initialization
                if len(state) == 0:
                    # Exponential moving average of gradient values
                    state['exp_avg'] = torch.zeros_like(p.data)
                    # Exponential moving average of squared gradient values
                    state['exp_avg_sq'] = torch.zeros_like(p.data)

                if p.dtype == torch.float16:
                    g_16.append(p.grad.data)
                    p_16.append(p.data)
                    m_16.append(state['exp_avg'])
                    v_16.append(state['exp_avg_sq'])
                elif p.dtype == torch.float32:
                    g_32.append(p.grad.data)
                    p_32.append(p.data)
                    m_32.append(state['exp_avg'])
                    v_32.append(state['exp_avg_sq'])
                else:
                    raise RuntimeError('FusedAdam only support fp16 and fp32.')

            if (len(g_16) > 0):
                self._multi_tensor_applier(self._multi_tensor_adam,
                                           self._dummy_overflow_buf,
                                           [g_16,
                                           p_16,
                                           m_16,
                                           v_16],
                                           group['lr'],
                                           beta1,
                                           beta2,
                                           group['eps'],
                                           group['step'],
                                           self._adam_w_mode,
                                           bias_correction,
                                           group['weight_decay'])
            if (len(g_32) > 0):
                self._multi_tensor_applier(self._multi_tensor_adam,
                                           self._dummy_overflow_buf,
                                           [g_32,
                                           p_32,
                                           m_32,
                                           v_32],
                                           group['lr'],
                                           beta1,
                                           beta2,
                                           group['eps'],
                                           group['step'],
                                           self._adam_w_mode,
                                           bias_correction,
                                           group['weight_decay'])

        return loss
