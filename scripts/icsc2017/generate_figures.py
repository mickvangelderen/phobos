#!/usr/bin/env python
# -*- coding: utf-8 -*-
import os
import sys
import numpy as np
import matplotlib.pyplot as plt
from matplotlib2tikz import save as tikz_save
import seaborn as sns
sns.set_style('whitegrid')
sns.set_context('paper')

file_dir = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.abspath(os.path.join(file_dir, os.pardir)))
from plot_sim import plot_states
from load_sim import load_messages, get_records_from_messages, get_time_vector
from simulate_whipple_benchmark import simulate


if __name__ == '__main__':
    messages = load_messages(os.path.join(file_dir, 'whipple.pb'))
    start_index = 50 # ignore some samples to give Kalman state estimate
                     # time to converge
    sim_time = 3
    stride = 10 # don't plot all the points

    records = get_records_from_messages(messages)[start_index::stride]
    t = get_time_vector(records)
    t -= t[0] # redefine zero time for simulator data

    # plot in degrees because understanding radians is harder
    states = records.state[:, 1:]
    fig1, ax1 = plot_states(t, states, second_yaxis=True,
                            to_degrees=True)

    m = messages[0]
    v = m.model.v
    dt = m.model.dt * stride
    n = int(sim_time/dt)
    x0 = np.array(messages[start_index].state.x[1:]).reshape((4, 1))
    x = simulate(v, x0, dt, n)
    t = np.array(range(n + 1)) * dt
    fig2, ax2 = plot_states(t, np.hstack((x0, x)).T, second_yaxis=True,
                            to_degrees=True)

    # set the axes x limits to be the same
    ax1[0].set_xlim([0, sim_time])
    ax2[0].set_xlim([0, sim_time])

    # center y-axis in figure

    def center_y(ax1, ax2, ax_num):
        lims = ax1[ax_num].get_ylim() + ax2[ax_num].get_ylim()
        mag = max(map(abs, lims))
        ax1[ax_num].set_ylim([-mag, mag])
        ax2[ax_num].set_ylim([-mag, mag])
    center_y(ax1, ax2, 0)
    center_y(ax1, ax2, 1)

    # redefine label names, this requires the latex package siunitx
    ax1[0].set_ylabel(r'\si{\degree}')
    ax2[0].set_ylabel(r'\si{\degree}')
    ax1[1].set_ylabel(r'\si{\degree\per\second}')
    ax2[1].set_ylabel(r'\si{\degree\per\second}')
    ax1[0].set_xlabel(r'time [\si{\second}]')
    ax2[0].set_xlabel(r'time [\si{\second}]')

    # redefine legend entries, this requires the latex package siunitx
    # enable legend frame, seaborn turns this off
    def redefine_legend_entries(ax):
        handles1, _ = ax[0].get_legend_handles_labels()
        handles2, _ = ax[1].get_legend_handles_labels()
        for axis in ax:
            try:
                axis.legend_.remove()
            except AttributeError:
                pass
        ax[0].legend(handles1 + handles2,
                     [r'$\phi$', r'$\delta$',
                      r'$\dot{\phi}$', r'$\dot{\delta}$'],
                     frameon=True, ncol=2, loc='lower right')
    redefine_legend_entries(ax1)
    redefine_legend_entries(ax2)
    # tikz_save doesn't support multiple y axes so the tex file still needs to
    # be modified manually to fix the legend and remove the arrowhead from the
    # second y axis =/

    # set figure size and save
    fig1.tight_layout()
    fig2.tight_layout()
    fh = '4cm'
    fw = '6.5cm'
    tikz_save('state_plot_simulator.tex', fig1,
              figureheight=fh, figurewidth=fw)
    tikz_save('state_plot_simulation.tex', fig2,
              figureheight=fh, figurewidth=fw)
    plt.show()
